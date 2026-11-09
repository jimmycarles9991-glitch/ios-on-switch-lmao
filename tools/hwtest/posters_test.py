"""posters.py over synthetic PosterBoard snapshots: ATX files of constant-colour ASTC blocks, decoded by sips."""

import plistlib
import struct
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path

from tools.hwtest import posters

WIDTH, HEIGHT = 72, 128
GL_COMPRESSED_RGBA_ASTC_4X4 = 0x93B0
# An LDR void-extent block: one colour for the block, no extent.
VOID_EXTENT = bytes.fromhex('fcfdffffffffffff')


def block(r: float, g: float, b: float, a: float) -> bytes:
    return VOID_EXTENT + struct.pack('<4H', *(round(c * 0xFFFF) for c in (r, g, b, a)))


def atx(colour: Callable[[float, float], tuple[float, ...]]) -> bytes:
    """Build an ATX file whose 4x4 block at panel point (x, y) is colour(x / WIDTH, y / HEIGHT)."""
    payload = b''.join(
        block(*colour(bx * 4 / WIDTH, by * 4 / HEIGHT)) for by in range(HEIGHT // 4) for bx in range(WIDTH // 4)
    )
    head = struct.pack(
        '<21I', 1, 0, 0, 0, GL_COMPRESSED_RGBA_ASTC_4X4, 0x1908, WIDTH, HEIGHT, 1, 0, 1, 1, 1, 0, 1, *([0] * 4), 3, 1
    )
    chunks = [(b'HEAD', head), (b'astc', struct.pack('<I', 0) + payload), (b'END ', b'')]
    return posters.ATX_MAGIC + b''.join(struct.pack('<I', len(data)) + tag + data for tag, data in chunks)


def petals(_x: float, y: float) -> tuple[float, ...]:
    """Draw Celosia's portrait composition: dark sky above, bright petals across the panel's bottom 55%."""
    return (0.9, 0.8, 0.7, 1.0) if y >= 0.45 else (0.1, 0.1, 0.2, 1.0)


def floating(_x: float, y: float) -> tuple[float, ...]:
    return (0.9, 0.8, 0.7, 1.0) if y >= 0.45 else (0.0, 0.0, 0.0, 0.0)


def landscape_petals(x: float, _y: float) -> tuple[float, ...]:
    """Draw a landscape composition in the panel's frame (orientation 4): petals along the held screen's bottom."""
    return (0.9, 0.8, 0.7, 1.0) if x >= 0.7 else (0.1, 0.1, 0.2, 1.0)


def landscape_floating(x: float, _y: float) -> tuple[float, ...]:
    return (0.9, 0.8, 0.7, 1.0) if x >= 0.7 else (0.0, 0.0, 0.0, 0.0)


def flat(_x: float, _y: float) -> tuple[float, ...]:
    return (0.1, 0.1, 0.15, 1.0)


class Store:
    """A Data volume with one poster configuration in PosterBoard's data store."""

    def __init__(self, root: Path):
        self.version = (
            root
            / posters.CONTAINERS
            / 'A'
            / posters.STORE
            / '61/Extensions/com.apple.MercuryPoster'
            / 'configurations/C/versions/0'
        )

    def bundle(self, name: str, size: str, created: str, levels: dict, *, orientation: int = 1) -> None:
        pks = self.version / posters.BUNDLES / name / 'Snapshot.pks'
        (pks / 'Contents').mkdir(parents=True)
        (pks / 'Resources').mkdir()
        info = {
            'AssetSize': size,
            'DateCreated': created,
            'DeviceInterfaceOrientation': orientation,
            'InterfaceOrientation': 1,
            'UserInterfaceStyle': 2,
        }
        (pks / 'Contents/Info.plist').write_bytes(plistlib.dumps({posters.INFO_KEY + k: v for k, v in info.items()}))
        for level, colour in levels.items():
            (pks / 'Resources' / f'{level}.atx').write_bytes(atx(colour))


class Posters(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.store = Store(self.root)

    def tearDown(self):
        self.tmp.cleanup()

    def test_atx_decodes_to_its_colours(self):
        path = self.root / 'level.atx'
        path.write_bytes(atx(petals))
        image = posters.decode(path)
        self.assertEqual((image.width, image.height), (WIDTH, HEIGHT))
        for (x, y), want in (((2, HEIGHT - 2), (230, 204, 179, 255)), ((2, 2), (26, 26, 51, 255))):
            got = image.pixel(x, y)
            self.assertTrue(all(abs(a - b) <= 1 for a, b in zip(got, want, strict=True)), (got, want))

    def test_boot_15_reuses_the_portrait_composition_over_the_time(self):
        self.store.bundle('P', '{576, 1024}', '2026-11-09T03:15:06Z', {'Composite': petals, 'Floating': floating})
        self.store.bundle(
            'L', '{1024, 576}', '2026-11-09T03:14:57Z', {'Composite': petals, 'Floating': floating}, orientation=4
        )
        got = posters.lock_poster(self.root)
        self.assertEqual(got['portrait_correlation'], 1.0)
        self.assertGreater(got['time_band_covered'], 0.4)
        self.assertIn('REUSES the portrait composition', posters.summary_line(got))

    def test_a_landscape_composition_leaves_the_time_clear(self):
        self.store.bundle('P', '{576, 1024}', '2026-11-09T03:15:06Z', {'Composite': petals, 'Floating': floating})
        self.store.bundle(
            'L',
            '{1024, 576}',
            '2026-11-09T03:14:57Z',
            {'Composite': landscape_petals, 'Floating': landscape_floating},
            orientation=4,
        )
        got = posters.lock_poster(self.root)
        self.assertLess(got['portrait_correlation'], posters.SAME_COMPOSITION)
        self.assertEqual(got['time_band_covered'], 0.0)
        self.assertIn('composed for landscape', posters.summary_line(got))

    def test_a_blank_render_is_passed_over(self):
        self.store.bundle('P', '{576, 1024}', '2026-11-09T03:15:06Z', {'Composite': petals})
        self.store.bundle('L1', '{1024, 576}', '2026-11-09T03:14:57Z', {'Composite': petals}, orientation=4)
        self.store.bundle('L2', '{1024, 576}', '2026-11-09T03:15:05Z', {'Composite': flat}, orientation=4)
        self.assertEqual(posters.lock_poster(self.root)['created'], '2026-11-09T03:14:57Z')

    def test_no_landscape_render_is_no_answer(self):
        self.store.bundle('P', '{576, 1024}', '2026-11-09T03:15:06Z', {'Composite': petals})
        self.assertIsNone(posters.observe(self.root))
        self.assertIsNone(posters.summary_line(None))


if __name__ == '__main__':
    unittest.main()
