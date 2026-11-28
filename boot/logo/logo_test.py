"""The panel from hekate's boot logo to backboardd's, pixel for pixel, from the built pictures.

hekate and ProgressUI are modelled from their code, not from logo.py: hekate's main.c _auto_launch and
gfx_render_bmp_argb (v6.5.3), and ProgressUI's -[PUIProgressWindow _collectDisplayInfo], _createLayer and
_layoutScreen in 21H461's dyld_shared_cache_arm64. U-Boot, the loader and XNU in graphics mode draw nothing between
them, so the panel hekate leaves is the panel ProgressUI must draw.
"""

import json
import math
import re
import struct
import unittest
import zlib
from pathlib import Path

from loader_abi import VIDEO
from python.runfiles import runfiles

PANEL = (720, 1280)
APPLE_GLYPH_BOX = (94, 207, 58, 198)
PROGRESSUI = '/System/Library/PrivateFrameworks/ProgressUI.framework'
LOGO_SOURCE = 'build/progressui/apple-logo~ipad.png'


def read_bmp(data: bytes) -> tuple[int, int, int, bytes]:
    """Read a BMP as hekate does: size, width, height and the pixel bytes after the header, or main.c's refusal."""
    size, offset = struct.unpack_from('<I', data, 2)[0], struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<II', data, 18)
    if data[:2] != b'BM' or data[28] != 32 or width > PANEL[0] or height > PANEL[1]:
        raise ValueError('hekate refuses this BMP')
    if size > len(data) or size - offset >= 4 << 20:
        raise ValueError('hekate refuses this BMP size')
    return size, width, height, data[offset:size]


def hekate_panel(bmp: bytes) -> list[int]:
    """Draw a boot entry's logopath BMP as hekate does: the framebuffer's 720x1280 words, rows 720 apart."""
    _, width, height, pixels = read_bmp(bmp)
    words = struct.unpack(f'<{width * height}I', pixels[: width * height * 4])
    x0, y0 = (PANEL[0] - width) >> 1, (PANEL[1] - height) >> 1
    fb = [words[0] if (width, height) != PANEL else 0] * (PANEL[0] * PANEL[1])
    for y in range(y0, y0 + height):
        for x in range(x0, x0 + width):
            fb[x + y * PANEL[0]] = words[(height + y0 - 1 - y) * width + x - x0]
    return [w & 0xFFFFFF for w in fb]


def paeth(a: int, b: int, c: int) -> int:
    """Predict a byte as PNG's Paeth filter does."""
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def unfilter(raw: bytes, width: int, height: int, channels: int) -> list[bytes]:
    """Undo the filters of a non-interlaced 8-bit image's scanlines."""
    stride, rows, previous = width * channels, [], bytearray(width * channels)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
        for i in range(stride):
            a, b = line[i - channels] if i >= channels else 0, previous[i]
            c = previous[i - channels] if i >= channels else 0
            line[i] = (line[i] + (0, a, b, (a + b) // 2, paeth(a, b, c))[kind]) & 255
        rows.append(bytes(line))
        previous = line
    return rows


def decode_png(data: bytes) -> tuple[int, int, int, list[bytes], list[bytes]]:
    """Decode an 8-bit non-interlaced RGB or RGBA PNG: width, height, colour type, chunk kinds and rows."""
    at, kinds, idat = 8, [], b''
    while at < len(data):
        length, kind = struct.unpack_from('>I4s', data, at)
        body = data[at + 8 : at + 8 + length]
        kinds.append(kind)
        if kind == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        at += 12 + length
    if depth != 8 or interlace or colour not in {2, 6}:
        raise ValueError('not an 8-bit non-interlaced RGB(A) PNG')
    return width, height, colour, kinds, unfilter(zlib.decompress(idat), width, height, 3 if colour == 2 else 4)


def display_boot_rotation() -> float:
    """Read /chosen/display-boot-rotation as tools/package/adt_edit.py writes it for the portrait-native kernel."""
    source = Path(runfiles.Create().Rlocation('_main/tools/package/adt_edit.py')).read_text()
    found = re.findall(r"'display-boot-rotation', u32\((\d+)\)", source)
    if len(found) != 1:
        raise ValueError('adt_edit.py: display-boot-rotation not found once')
    return float(found[0])


def progressui_orientation(rotation: float) -> float:
    """Turn DisplayBootRotation into the layer orientation, as _collectDisplayInfo does (main-screen-orientation 0)."""
    for degrees, turn in ((90.0, 3 * math.pi / 2), (180.0, math.pi), (270.0, math.pi / 2)):
        if abs(rotation - degrees) < 0.01:
            return turn
    return 0.0


def progressui_panel(png: bytes, rotation: float) -> list[int]:
    """Render ProgressUI's Apple logo window: the 720x1280 frame as 0xRRGGBB words, rows 720 apart.

    The root layer has the display's bounds (swapped when sideways), black, at the framebuffer's centre, with the
    affine transform CGAffineTransformMakeRotation(-orientation); the logo layer's frame is the image's own size at
    ceil((bounds - size) / 2). Each framebuffer pixel centre is taken back through the transform to a logo texel.
    """
    width, height, _, _, rows = decode_png(png)
    angle = progressui_orientation(rotation)
    bounds = (PANEL[1], PANEL[0]) if abs(math.cos(angle)) < 0.5 else PANEL
    left, top = math.ceil((bounds[0] - width) * 0.5), math.ceil((bounds[1] - height) * 0.5)
    a, b = math.cos(-angle), math.sin(-angle)
    fb = []
    for row in range(PANEL[1]):
        for column in range(PANEL[0]):
            qx, qy = column + 0.5 - PANEL[0] * 0.5, row + 0.5 - PANEL[1] * 0.5
            u = math.floor(a * qx + b * qy + bounds[0] * 0.5 - left)
            v = math.floor(-b * qx + a * qy + bounds[1] * 0.5 - top)
            fb.append(rows[v][3 * u] * 0x010101 if 0 <= u < width and 0 <= v < height else 0)
    return fb


def lit_box(rows: list[bytes], channels: int) -> tuple[int, int, int, int]:
    """Find the leftmost, rightmost, top and bottom lit pixel of an image."""
    lit = [(x, y) for y, row in enumerate(rows) for x in range(len(row) // channels) if row[channels * x]]
    return min(x for x, _ in lit), max(x for x, _ in lit), min(y for _, y in lit), max(y for _, y in lit)


class BootLogo(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        location = runfiles.Create().Rlocation
        files = {
            name: Path(location(f'_main/boot/logo/{name}')).read_bytes()
            for name in ('apple-logo~ipad.png', 'bootlogo.bmp', 'icon_ios_hue.bmp')
        }
        cls.png, cls.bootlogo, cls.icon = files['apple-logo~ipad.png'], files['bootlogo.bmp'], files['icon_ios_hue.bmp']
        cls.progressui = progressui_panel(cls.png, display_boot_rotation())

    def test_hekate_and_progressui_draw_the_same_panel(self) -> None:
        hekate = hekate_panel(self.bootlogo)
        wrong = [i for i, (a, b) in enumerate(zip(hekate, self.progressui, strict=True)) if a != b]
        first = f', the first at column {wrong[0] % PANEL[0]} row {wrong[0] // PANEL[0]}' if wrong else ''
        self.assertEqual(len(wrong), 0, f'{len(wrong)} of {len(hekate)} pixels differ{first}')
        self.assertGreater(sum(1 for w in hekate if w), 10000)

    def test_a_logo_one_pixel_off_is_caught(self) -> None:
        bmp = bytearray(self.bootlogo)
        offset = struct.unpack_from('<I', bmp, 10)[0]
        bmp[offset:] = bmp[offset + 4 :] + bmp[offset : offset + 4]
        self.assertNotEqual(hekate_panel(bytes(bmp)), self.progressui)

    def test_progressui_turns_landscape_as_the_console_does(self) -> None:
        width, height, _, _, rows = decode_png(self.png)
        left, top = math.ceil((PANEL[1] - width) / 2), math.ceil((PANEL[0] - height) / 2)
        for y in range(top, top + height):
            for x in range(left, left + width):
                panel = self.progressui[(PANEL[1] - 1 - x) * PANEL[0] + y]
                self.assertEqual(panel, rows[y - top][3 * (x - left)] * 0x010101, f'landscape ({x}, {y})')

    def test_logo_art_is_upright(self) -> None:
        _, _, _, _, rows = decode_png(self.png)
        left, right, top, bottom = lit_box(rows, 3)
        leaf = [x for x in range(left, right + 1) if rows[top][3 * x]]
        self.assertGreater(min(leaf), (left + right) / 2, 'the leaf leans right at the top')
        self.assertGreater(bottom - top, right - left)

    def test_progressui_logo_is_opaque_untagged_grey_at_apples_size(self) -> None:
        width, height, colour, kinds, rows = decode_png(self.png)
        self.assertEqual((width, height, colour), (300, 300, 2))
        self.assertEqual(kinds, [b'IHDR', b'IDAT', b'IEND'])
        box = lit_box(rows, 3)
        for ours, apples in zip(box, APPLE_GLYPH_BOX, strict=True):
            self.assertLessEqual(abs(ours - apples), 1, f'glyph box {box}, Apple art {APPLE_GLYPH_BOX}')
        self.assertTrue(all(row[i] == row[i + 1] == row[i + 2] for row in rows for i in range(0, len(row), 3)))

    def test_icon_is_what_nyx_reads(self) -> None:
        size, width, height, _ = read_bmp(self.icon)
        self.assertEqual((size, width, height), (len(self.icon), 192, 192))

    def test_icon_is_a_hue_icon_as_nyx_draws_its_own(self) -> None:
        _, _, _, pixels = read_bmp(self.icon)
        self.assertEqual({pixels[i : i + 3] for i in range(0, len(pixels), 4)}, {b'\xff\xff\xff'})
        lit = [i // 4 for i in range(0, len(pixels), 4) if pixels[i + 3] > 127]
        xs, ys = [i % 192 for i in lit], [i // 192 for i in lit]
        width, height = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
        self.assertLessEqual(abs(max(width, height) - 160), 1, f'glyph {width}x{height}')
        self.assertLessEqual(abs(min(xs) + max(xs) - 191), 1)
        self.assertLessEqual(abs(min(ys) + max(ys) - 191), 1)

    def test_progressui_finds_this_logo_at_scale_1_and_2(self) -> None:
        manifest = json.loads(Path(runfiles.Create().Rlocation('_main/overlay/manifest.json')).read_text())
        overlay = {entry['path']: entry['source'] for entry in manifest['files']}
        for scale in (1, 2):
            first = f'{PROGRESSUI}/apple-logo@{scale}x~ipad.png'
            self.assertEqual(overlay.get(first), LOGO_SOURCE, f'{first} is the first name tried at scale {scale}')
        self.assertEqual(overlay.get(f'{PROGRESSUI}/apple-logo~ipad.png'), LOGO_SOURCE)

    def test_progressui_finds_this_logo_for_j120s_class_with_usb_c(self) -> None:
        manifest = json.loads(Path(runfiles.Create().Rlocation('_main/overlay/manifest.json')).read_text())
        j320 = {e['path']: e for e in manifest['files'] if '/apple-logo-j320' in e['path']}
        for name in ('apple-logo-j320@1x~ipad.png', 'apple-logo-j320@2x~ipad.png', 'apple-logo-j320~ipad.png'):
            entry = j320.get(f'{PROGRESSUI}/{name}')
            self.assertIsNotNone(entry, name)
            self.assertEqual(entry['source'], LOGO_SOURCE)
            self.assertEqual(entry.get('kind'), '21H461')

    def test_loader_hands_xnu_graphics_mode(self) -> None:
        self.assertEqual(VIDEO[1], 1, 'Video.v_display 0 is text mode: XNU clears the panel for its console')


if __name__ == '__main__':
    unittest.main()
