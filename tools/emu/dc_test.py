"""Display controller A of the patched tegra_qemu under qtest.

The register latch, the CRC unit, block-linear scanout, blending and SMMU translation, each checked through the CRC
that SwitchDisplay's probes read.
"""

import tempfile
import unittest
from pathlib import Path

from tools.emu import qemu
from tools.emu.qtest import Qtest

DC = 0x54200000
MC = 0x70019000
FRAME_NS = 1_000_000_000 // 60 + 1
WIDTH, HEIGHT = 64, 32
STRIDE = WIDTH * 4
PITCH_A, BLOCK_A, PITCH_B, TABLES = 0x90000000, 0x90100000, 0x90200000, 0x90300000
IOVA = 0x40000000

STATE_ACCESS, STATE_CONTROL, WINDOW_HEADER, DISPLAY_COMMAND = 0x040, 0x041, 0x042, 0x032
CRC_CONTROL, CRC_LATCHED, DISP_ACTIVE, BACKGROUND = 0x300, 0x329, 0x409, 0x4E4
WIN = {'A': (0xB80, 0xBC0), 'B': (0xD80, 0xDC0)}
ACT_REQ = {'A': 1 << 1, 'B': 1 << 2}
GENERAL_ACT_REQ, CRC_ON, BYPASS = 1 << 0, (1 << 3) | (1 << 2) | 1, 1 << 24
YUV_ON = 1 << 30 | 1 << 18
# WINC_CSC_YOF..KVB: BT.601 video range (NativePlanes.h kCsc).
CSC_BT601 = (0x0F0, 0x12A, 0x000, 0x199, 0x39C, 0x330, 0x204, 0x000)


def image(seed: int) -> bytes:
    return bytes((x * 7 + y * 13 + seed) & 0xFF for y in range(HEIGHT) for x in range(STRIDE))


def gob_offset(x: int, y: int) -> int:
    return ((x >> 5) & 1) << 8 | ((y >> 1) & 3) << 6 | ((x >> 4) & 1) << 5 | (y & 1) << 4 | (x & 15)


def block_linear(pitch: bytes, height_log2: int) -> bytes:
    rows, block = 8 << height_log2, 512 << height_log2
    out = bytearray(-(-HEIGHT // rows) * (STRIDE // 64) * block)
    for y in range(HEIGHT):
        for x in range(0, STRIDE, 16):
            at = (y // rows) * (STRIDE // 64) * block + (x // 64) * block + ((y % rows) // 8) * 512
            at += gob_offset(x % 64, y % 8)
            out[at : at + 16] = pitch[y * STRIDE + x : y * STRIDE + x + 16]
    return bytes(out)


def block_linear_plane(pitch: bytes, stride: int, rows: int, height_log2: int) -> bytes:
    """Lay a pitch plane `stride` bytes wide out in GM20B's GOBs, blocks 1 << height_log2 GOBs tall."""
    lines, block = 8 << height_log2, 512 << height_log2
    out = bytearray(-(-rows // lines) * (stride // 64) * block)
    for y in range(rows):
        for x in range(0, stride, 16):
            at = (y // lines) * (stride // 64) * block + (x // 64) * block + ((y % lines) // 8) * 512
            at += gob_offset(x % 64, y % 8)
            out[at : at + 16] = pitch[y * stride + x : y * stride + x + 16]
    return bytes(out)


class Dc:
    def __init__(self, q: Qtest):
        self.q = q
        self.w(DISP_ACTIVE, HEIGHT << 16 | WIDTH)
        self.w(BACKGROUND, 0)
        self.w(DISPLAY_COMMAND, 1 << 5)

    def w(self, word: int, value: int) -> None:
        self.q.writel(DC + word * 4, value)

    def r(self, word: int) -> int:
        return self.q.readl(DC + word * 4)

    def window(self, name: str, start: int, kind: int = 0, blend: int = BYPASS | 255, factors: int = 0) -> None:
        win, buf = WIN[name]
        for offset, value in (
            (0x03, 12),
            (0x04, 0),
            (0x05, HEIGHT << 16 | WIDTH),
            (0x06, HEIGHT << 16 | STRIDE),
            (0x07, 0),
            (0x08, 0),
            (0x09, 0x1000 << 16 | 0x1000),
            (0x0A, STRIDE),
            (0x16, blend),
            (0x18, factors),
            (0x00, 1 << 30),
        ):
            self.w(win + offset, value)
        for offset, value in ((0x00, start), (0x06, 0), (0x08, 0), (0x0B, kind), (0x0D, 0)):
            self.w(buf + offset, value)

    def commit(self, *windows: str) -> None:
        bits = GENERAL_ACT_REQ
        for name in windows:
            bits |= ACT_REQ[name]
        self.w(STATE_CONTROL, bits << 8)
        self.w(STATE_CONTROL, bits)

    def frames(self, count: int) -> list[int]:
        crcs = []
        for _ in range(count):
            self.q.step(FRAME_NS)
            crcs.append(self.r(CRC_LATCHED))
        return crcs


@unittest.skipUnless(qemu.ever_built(), 'this Mac has never built tegra_qemu')
class DcTest(unittest.TestCase):
    def setUp(self) -> None:
        binary = qemu.built()
        if not binary:
            self.skipTest('this Mac has no tegra_qemu build of these patches: bazel run //tools/emu:build')
        self.log = Path(tempfile.mkdtemp()) / 'qemu.log'
        self.q = Qtest(binary, self.log)
        self.dc = Dc(self.q)
        self.q.write(PITCH_A, image(0))
        self.dc.window('A', PITCH_A)
        self.dc.commit('A')
        self.dc.frames(1)

    def tearDown(self) -> None:
        self.q.close()

    def crc_on(self) -> int:
        self.dc.w(CRC_CONTROL, CRC_ON)
        self.dc.commit()
        return self.dc.frames(4)[-1]

    def test_act_req_reads_set_until_the_frame_boundary(self) -> None:
        win, buf = WIN['A']
        self.dc.w(buf, PITCH_B)
        self.dc.commit('A')
        self.assertEqual(self.dc.r(STATE_CONTROL), GENERAL_ACT_REQ | ACT_REQ['A'])
        self.dc.w(STATE_ACCESS, 1)
        self.assertEqual(self.dc.r(buf), PITCH_A)
        self.dc.frames(1)
        self.assertEqual(self.dc.r(STATE_CONTROL), 0)
        self.assertEqual(self.dc.r(buf), PITCH_B)
        self.dc.w(STATE_ACCESS, 0)
        self.assertEqual(self.dc.r(win), 1 << 30)

    def test_stopped_display_latches_at_once(self) -> None:
        self.dc.w(DISPLAY_COMMAND, 0)
        self.dc.w(WIN['A'][1], PITCH_B)
        self.dc.commit('A')
        self.assertEqual(self.dc.r(STATE_CONTROL), 0)

    def test_crc_latches_at_the_second_frame_end_and_holds(self) -> None:
        self.dc.w(CRC_CONTROL, CRC_ON)
        self.dc.commit()
        reads = self.dc.frames(6)
        self.assertEqual(reads[0], 0)
        self.assertNotEqual(reads[1], 0)
        self.assertEqual(len(set(reads[1:])), 1)

    def test_crc_follows_one_pixel(self) -> None:
        base = self.crc_on()
        pixel = self.q.readl(PITCH_A)
        self.q.writel(PITCH_A, pixel ^ 0x00010101)
        self.assertNotEqual(self.dc.frames(3)[-1], base)
        self.q.writel(PITCH_A, pixel)
        self.assertEqual(self.dc.frames(3)[-1], base)

    def test_block_linear_scans_as_pitch(self) -> None:
        base = self.crc_on()
        for height_log2 in (0, 1, 2):
            with self.subTest(block_height=height_log2):
                self.q.write(BLOCK_A, block_linear(image(0), height_log2))
                self.dc.w(WIN['A'][1], BLOCK_A)
                self.dc.w(WIN['A'][1] + 0x0B, 2 | height_log2 << 4)
                self.dc.commit('A')
                self.assertEqual(self.dc.frames(4)[-1], base)

    def test_scan_column_turns_the_source(self) -> None:
        columns, rows = HEIGHT, WIDTH
        source = image(3)[: columns * rows * 4]
        turned = bytearray(STRIDE * HEIGHT)
        for line in range(HEIGHT):
            for pixel in range(WIDTH):
                at = (pixel * columns + columns - 1 - line) * 4
                turned[line * STRIDE + pixel * 4 : line * STRIDE + pixel * 4 + 4] = source[at : at + 4]
        self.q.write(PITCH_B, bytes(turned))
        self.dc.w(WIN['A'][1], PITCH_B)
        self.dc.commit('A')
        want = self.crc_on()
        win, buf = WIN['A']
        self.q.write(PITCH_A, source)
        self.dc.w(win + 0x06, columns << 16 | rows * 4)
        self.dc.w(win + 0x0A, columns * 4)
        self.dc.w(win, 1 << 30 | 1 << 4 | 1 << 0)
        self.dc.w(buf, PITCH_A)
        self.dc.w(buf + 0x06, columns * 4 - 1)
        self.dc.commit('A')
        self.assertEqual(self.dc.frames(4)[-1], want)

    def test_premultiplied_window_b_over_a(self) -> None:
        top = bytearray(image(1))
        for at in range(0, len(top), 8):
            top[at + 3] = 0xFF
            top[at + 4 : at + 8] = b'\0\0\0\0'
        self.q.write(PITCH_B, bytes(top))
        expected = bytearray(image(0))
        for at in range(0, len(top), 8):
            expected[at : at + 4] = top[at : at + 4]
        self.q.write(PITCH_A + 0x10000, bytes(expected))
        self.dc.w(WIN['A'][1], PITCH_A + 0x10000)
        self.dc.commit('A')
        want = self.crc_on()
        self.dc.w(WIN['A'][1], PITCH_A)
        self.dc.window('B', PITCH_B, blend=0x00FFFF00 | 0, factors=0x262)
        self.dc.commit('A', 'B')
        self.assertEqual(self.dc.frames(4)[-1], want)

    def ui_with_a_hole(self) -> bytes:
        """Return the composited UI, premultiplied: opaque but for a transparent hole the video shows through."""
        out = bytearray(image(5))
        for y in range(HEIGHT):
            for x in range(WIDTH):
                at = y * STRIDE + x * 4
                if 8 <= x < 56 and 4 <= y < 28:
                    out[at : at + 4] = b'\0\0\0\0'
                else:
                    out[at + 3] = 0xFF
        return bytes(out)

    def column_scan(self, start: int, columns: int, rows: int) -> None:
        win, buf = WIN['A']
        self.dc.window('A', start, blend=BYPASS | 1)
        self.dc.w(win + 0x06, columns << 16 | rows * 4)
        self.dc.w(win + 0x0A, columns * 4)
        self.dc.w(win, 1 << 30 | 1 << 4 | 1 << 0)
        self.dc.w(buf + 0x06, columns * 4 - 1)

    def test_landscape_change_latches_both_windows_at_one_boundary(self) -> None:
        # Portrait: the UI on A in front of the video's turned copy on B. Landscape (SwitchLandscapeDisplay.h): the
        # same UI on B in front of the video column-scanned on A, both windows in one request. The two show the same
        # picture, so every frame across the change must have its CRC; a half-taken change would not.
        columns, rows = HEIGHT, WIDTH
        source = image(3)[: columns * rows * 4]
        turned = bytearray(STRIDE * HEIGHT)
        for line in range(HEIGHT):
            for pixel in range(WIDTH):
                at = (pixel * columns + columns - 1 - line) * 4
                turned[line * STRIDE + pixel * 4 : line * STRIDE + pixel * 4 + 4] = source[at : at + 4]
        ui, video = PITCH_A + 0x20000, BLOCK_A
        self.q.write(ui, self.ui_with_a_hole())
        self.q.write(PITCH_B, bytes(turned))
        self.q.write(video, source)
        over = {'blend': 0x00FFFF00 | 0, 'factors': 0x262}
        self.dc.window('A', ui, **over)
        self.dc.window('B', PITCH_B, blend=BYPASS | 1)
        self.dc.commit('A', 'B')
        want = self.crc_on()
        self.dc.window('B', ui, **over)
        self.column_scan(video, columns, rows)
        self.dc.commit('A', 'B')
        self.assertEqual(self.dc.frames(6), [want] * 6)
        # The control: window A taken alone shows the UI over itself, not the picture.
        self.dc.window('A', ui, **over)
        self.dc.window('B', PITCH_B, blend=BYPASS | 1)
        self.dc.commit('A')
        self.assertNotEqual(self.dc.frames(3)[-1], want)
        self.dc.commit('B')
        self.assertEqual(self.dc.frames(3)[-1], want)

    def yuv_window(self, name: str, start: int, chroma: int, size: tuple[int, int, int], options: int) -> None:
        """Program `name` for NV12 (prescaled, line stride, kind) with BT.601 video range in its CSC."""
        prescaled, stride, kind = size
        win, buf = WIN[name]
        csc = 0xA00 + 0x200 * 'ABC'.index(name) + 0x111
        for i, value in enumerate(CSC_BT601):
            self.dc.w(csc + i, value)
        for offset, value in (
            (0x03, 43),
            (0x04, 0),
            (0x05, HEIGHT << 16 | WIDTH),
            (0x06, prescaled),
            (0x07, 0),
            (0x08, 0),
            (0x09, 0x1000 << 16 | 0x1000),
            (0x0A, stride),
            (0x16, BYPASS | 1),
            (0x18, 0),
            (0x00, options),
        ):
            self.dc.w(win + offset, value)
        for offset, value in ((0x00, start), (0x02, chroma), (0x06, 0), (0x08, 0), (0x0B, kind), (0x0D, 0)):
            self.dc.w(buf + offset, value)

    def test_landscape_change_latches_nv12_column_scan_with_the_ui(self) -> None:
        # The shipped format: the decoder's NV12 in 2-GOB blocks column-scanned on window A (its CSC written while A
        # still scans the RGB UI), against the same picture turned in memory on window B. Every frame across the one
        # request for both windows shows the same picture; window A requested alone does not.
        columns, rows = HEIGHT, WIDTH
        luma = bytes((x * 7 + y * 13 + 5) & 0xFF if x < columns else 0 for y in range(rows) for x in range(64))
        pairs = bytes(
            (32 + bx * 11 + by * 5 + c * 70) & 0xFF if bx < columns // 2 else 0
            for by in range(rows // 2)
            for bx in range(32)
            for c in range(2)
        )
        turned_luma = bytes(luma[x * 64 + columns - 1 - y] for y in range(HEIGHT) for x in range(WIDTH))
        turned_pairs = bytes(
            pairs[(x // 2) * 64 + ((columns - 1 - 2 * yc) // 2) * 2 + c]
            for yc in range(HEIGHT // 2)
            for x in range(0, WIDTH, 2)
            for c in range(2)
        )
        ui, source, copy = PITCH_A + 0x20000, BLOCK_A, PITCH_B
        source_chroma, copy_chroma = source + 0x10000, copy + 0x10000
        self.q.write(ui, self.ui_with_a_hole())
        self.q.write(source, block_linear_plane(luma, 64, rows, 1))
        self.q.write(source_chroma, block_linear_plane(pairs, 64, rows // 2, 1))
        self.q.write(copy, turned_luma)
        self.q.write(copy_chroma, turned_pairs)
        over = {'blend': 0x00FFFF00 | 0, 'factors': 0x262}
        self.dc.window('A', ui, **over)
        self.yuv_window('B', copy, copy_chroma, (HEIGHT << 16 | WIDTH, WIDTH << 16 | WIDTH, 0), YUV_ON)
        self.dc.commit('A', 'B')
        want = self.crc_on()
        for i, value in enumerate(CSC_BT601):
            self.dc.w(0xA00 + 0x111 + i, value)
        self.assertEqual(self.dc.frames(2), [want] * 2)
        self.dc.window('B', ui, **over)
        self.yuv_window(
            'A', source, source_chroma, (columns << 16 | rows, 64 << 16 | 64, 2 | 1 << 4), YUV_ON | 1 << 4 | 1 << 0
        )
        self.dc.commit('A', 'B')
        self.assertEqual(self.dc.frames(6), [want] * 6)
        self.dc.window('A', ui, **over)
        self.yuv_window('B', copy, copy_chroma, (HEIGHT << 16 | WIDTH, WIDTH << 16 | WIDTH, 0), YUV_ON)
        self.dc.commit('A')
        self.assertNotEqual(self.dc.frames(3)[-1], want)
        self.dc.commit('B')
        self.assertEqual(self.dc.frames(3)[-1], want)

    def test_window_b_reads_through_the_smmu(self) -> None:
        self.q.write(PITCH_B, image(2))
        self.dc.window('B', PITCH_B, blend=BYPASS | 0)
        self.dc.commit('B')
        base = self.crc_on()
        directory, table = TABLES, TABLES + 0x1000
        self.q.writel(directory + (IOVA >> 22) * 4, (table >> 12) | 0xF0000000)
        for page in range(STRIDE * HEIGHT // 4096 + 1):
            self.q.writel(table + ((IOVA >> 12) + page) % 1024 * 4, ((PITCH_B >> 12) + page) | 0xA0000000)
        self.q.writel(MC + 0x10, 1)
        self.q.writel(MC + 0x1C, 126)
        self.q.writel(MC + 0x20, (directory >> 12) | 0xE0000000)
        self.q.writel(MC + 0x228, self.q.readl(MC + 0x228) | 1 << 3)
        self.q.writel(MC + 0x240, 1 << 31 | 126)
        self.dc.w(WIN['B'][1], IOVA)
        self.dc.commit('B')
        self.assertEqual(self.dc.frames(4)[-1], base)
        self.dc.w(WIN['B'][1], IOVA + 0x100000)
        self.dc.commit('B')
        self.assertNotEqual(self.dc.frames(4)[-1], base)
        self.assertIn('SMMU', self.log.read_text())


if __name__ == '__main__':
    unittest.main()
