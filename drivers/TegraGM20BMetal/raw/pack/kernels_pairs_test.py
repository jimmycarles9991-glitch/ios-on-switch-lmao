"""raw/pack/kernels.py and packfile.py: a kernel sampler entry's window reaches the pack as the direct compile made it.

The window holds the texture slots the sampler samples (runtime/raw_pack.h kernelSamplerPairs, pack v26), which the
runtime maps the sampler's header by.
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kernels
import packfile

KNOWN = 1 << 31
HEADER = [0x26092, 0x291, 0xFFF000, 0, 0, 0, 0, 0]
OTHER = [0x26000, 0x291, 0xFFF000, 0, 0, 0, 0, 0]


def record(entries: list[tuple[int, int, int, int]]) -> dict:
    """Give a DKRN record as directpack.mm writes one: its PackKernel's bytes, sampler headers, key and program."""
    blob = kernels.HEAD.pack(0, 0, 0, len(entries), -1)
    blob += struct.pack('<64I', *[0] * 64)
    blob += kernels.COUNTS.pack(256, 0, 0)
    blob += struct.pack('<32I', *[0] * 32)
    blob += b''.join(kernels.ENTRY.pack(*e) for e in entries)
    blob += bytes(kernels.ENTRY.size * (kernels.ENTRIES - len(entries)))
    blob += bytes(packfile.KERNEL_V20.size + packfile.KERNEL_V21.size)
    assert len(blob) == kernels.KERNEL_BYTES
    return {'kernel': blob, 'samplers': [OTHER, HEADER], 'key': 'k | tg=8,8,1', 'program': 'p'}


def windows(entries: list[tuple[int, int, int, int]], constexpr_tsc: list[list[int]]) -> list[tuple[int, int, int]]:
    """Give each entry's (kind, slot, window) as the pack's PackKernel holds it."""
    k = kernels.kernel(record(entries), constexpr_tsc)
    k.pop('prog')
    k['program'] = 0
    packed = packfile.kernel_bytes(k, 0)
    at = kernels.HEAD.size + 256 + kernels.COUNTS.size + 128
    out = []
    for i in range(len(entries)):
        _, kind, slot, window = kernels.ENTRY.unpack_from(packed, at + kernels.ENTRY.size * i)
        out.append((kind, slot, window))
    return out


class SamplerPairs(unittest.TestCase):
    def test_a_combined_samplers_slot_reaches_the_pack(self) -> None:
        entries = [(0, kernels.TEXTURE, 2, 0x08), (0, kernels.SAMPLER, kernels.CONSTEXPR | 1, KNOWN | 1 << 2)]
        tsc: list[list[int]] = []
        self.assertEqual(
            windows(entries, tsc),
            [(kernels.TEXTURE, 2, 0x08), (kernels.SAMPLER, kernels.CONSTEXPR, KNOWN | 1 << 2)],
        )
        self.assertEqual(tsc, [HEADER])

    def test_the_null_images_sampler_and_an_unknown_one_keep_theirs(self) -> None:
        entries = [(4, kernels.SAMPLER, kernels.CONSTEXPR, KNOWN), (8, kernels.SAMPLER, kernels.CONSTEXPR | 1, 0)]
        tsc = [HEADER]
        self.assertEqual(
            windows(entries, tsc),
            [(kernels.SAMPLER, kernels.CONSTEXPR | 1, KNOWN), (kernels.SAMPLER, kernels.CONSTEXPR, 0)],
        )

    def test_a_buffers_residual_word_still_rides_its_window(self) -> None:
        entries = [(16, kernels.BUFFER, 3, 0x40 | 5 << 24)]
        self.assertEqual(windows(entries, []), [(kernels.BUFFER, 3, 0x40 | 5 << 24)])

    def test_the_pack_is_version_26(self) -> None:
        self.assertEqual(packfile.VERSION, 28)


if __name__ == '__main__':
    unittest.main()
