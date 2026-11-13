"""gpu.ca.fetch_routes' 4x fetch on the host, its storage image bound under each header the raw path has bound.

testdata/fetch_order_msaa4.hex is NAK's fragment program of that pass's state (fetch_order_fragment into a 4x BGRA8
target, its storage form; directcheck's dump). It runs a pass a sample: PIXLD MyIndex gives the sample, TXQ the
header's sample count and the sample's position, and each SULD/SUST goes to the pixel's sample in the 2x2 grid of
samples a pixel (nak_nir_lower_tex.c lower_msaa_image_access). The surface unit bounds those by the header's WIDTH
and HEIGHT alone, so a pixel keeps the clear colour unless the stores of all its samples land inside them.

Under the attachment's texture header (WIDTH and HEIGHT in pixels, what Device::viewTic bound before) the picture is
boot 14's line: 3072 of 4096 wrong from (32, 0), only the top-left quarter drawn. Under NIL's storage header, the
sample grid's extent, every pixel is drawn. raw_view_test holds that the raw path's storage view is NIL's header.
"""

import struct
import unittest
from pathlib import Path

from tools.gpu.maxwell import isa
from tools.gpu.maxwell.interp import Inputs
from tools.gpu.maxwell.quad import run_quad

FIXTURE = Path(__file__).with_name('testdata') / 'fetch_order_msaa4.hex'
SIZE, SAMPLES = 64, 4
HANDLE = 0x10  # the storage image's word, at c[1][0x10]
PIXLD_MY_INDEX = 5
TXQ_TEXTURE_TYPE, TXQ_SAMPLER_POS = 2, 5
# NIL's headers of a 64x64 4x BGRA8 image, words 4 and 5 (raw/fx/test/nilimage.c ms; raw_view_test's)
TEXTURE_HEADER = (0xE080003F, 0x8000003F)
STORAGE_HEADER = (0xE080007F, 0x8000007F)


def f32(x: float) -> int:
    return struct.unpack('<I', struct.pack('<f', x))[0]


def extent(words: tuple[int, int]) -> tuple[int, int]:
    """WIDTH_MINUS_ONE (bits 143:128) and HEIGHT_MINUS_ONE (175:160) of header words 4 and 5, plus one."""
    return (words[0] & 0xFFFF) + 1, (words[1] & 0xFFFF) + 1


def sample_position(s: int) -> int:
    """Give TXQ SamplerPos of 2X2 sample `s`: NIL's sa_offset (s & 1, s >> 1), signed 4.12 about the centre."""
    x, y = ((s & 1) + 0.5) / 2 - 0.5, ((s >> 1) + 0.5) / 2 - 0.5
    return (int(x * 4096) & 0xFFFF) | (int(y * 4096) & 0xFFFF) << 16


def lane(x: int, y: int, s: int, loads: list) -> Inputs:
    """One invocation's inputs: pixel (x, y) at its centre, sample `s`; a lane's TXQ SamplerPos is its own sample's."""
    attributes = {0x70: f32(x + 0.5), 0x74: f32(y + 0.5)}

    def texture(op: tuple) -> list[int]:
        if op[0] == 'txq' and op[1] == HANDLE and op[2] == TXQ_TEXTURE_TYPE:
            return [0, 0, SAMPLES, 0]
        if op[0] == 'txq' and op[1] == HANDLE and op[2] == TXQ_SAMPLER_POS:
            return [sample_position(s), 0, 0, 0]
        if op[0] == 'suld' and op[1] == HANDLE:
            loads.append(op[2])
            return [0, 0, 0, 0]
        raise AssertionError(f'unexpected fetch {op}')

    def cbuf(bank: int, offset: int) -> int:
        return HANDLE if (bank, offset) == (1, 0x10) else 0

    return Inputs(lambda a: attributes.get(a, 0), cbuf, texture, pixel=lambda mode: s if mode == PIXLD_MY_INDEX else 0)


def stores() -> tuple[dict, bool]:
    """Each invocation's SUST coordinate, by (x, y, sample), and whether every SULD read the texel its SUST wrote."""
    code = isa.from_hex_dump(FIXTURE.read_text())
    out, same = {}, True
    for s in range(SAMPLES):
        for qy in range(0, SIZE, 2):
            for qx in range(0, SIZE, 2):
                pixels = [(qx, qy), (qx + 1, qy), (qx, qy + 1), (qx + 1, qy + 1)]
                loads = [[] for _ in pixels]
                quad = [lane(x, y, s, loads[i]) for i, (x, y) in enumerate(pixels)]
                for i, (_, killed, st) in enumerate(run_quad(code, quad, 0)):
                    assert not killed, (pixels[i], s)
                    assert len(st) == 1, (pixels[i], s, st)
                    assert st[0][0] == HANDLE, (pixels[i], s, st)
                    out[(*pixels[i], s)] = st[0][1]
                    same &= loads[i] == [st[0][1]]
    return out, same


def picture(stored: dict, width: int, height: int) -> tuple[int, tuple, str]:
    """RasterDiff's wrong count, first wrong pixel and 8x8 map of the pixels not every sample's store reached."""
    cells, wrong, first = [False] * 64, 0, None
    for y in range(SIZE):
        for x in range(SIZE):
            if all(stored[x, y, s][0] < width and stored[x, y, s][1] < height for s in range(SAMPLES)):
                continue
            wrong += 1
            first = first or (x, y)
            cells[(y * 8 // SIZE) * 8 + x * 8 // SIZE] = True
    grid = ''.join(f'{sum(c << (3 - k) for k, c in enumerate(cells[a : a + 4])):x}' for a in range(0, 64, 4))
    return wrong, first, grid


class MultisampledStorageView(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.stored, cls.same = stores()

    def test_each_sample_is_its_own_texel_of_the_sample_grid(self) -> None:
        self.assertTrue(self.same)
        texels = set(self.stored.values())
        self.assertEqual(len(texels), SIZE * SIZE * SAMPLES)
        self.assertEqual(texels, {(x, y) for x in range(2 * SIZE) for y in range(2 * SIZE)})

    def test_the_pixel_extent_header_draws_boot_14s_quarter(self) -> None:
        self.assertEqual(picture(self.stored, *extent(TEXTURE_HEADER)), (3072, (32, 0), '0f0f0f0fffffffff'))

    def test_the_sample_grid_header_draws_every_pixel(self) -> None:
        self.assertEqual(picture(self.stored, *extent(STORAGE_HEADER)), (0, None, '0000000000000000'))


if __name__ == '__main__':
    unittest.main()
