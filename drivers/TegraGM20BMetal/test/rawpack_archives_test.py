#!/usr/bin/env python3
"""archives.py: an archive inventory's declared states as the pack's PackArchives section (runtime/raw_pack.h)."""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raw', 'pack'))
import archives
import packfile

RENDERBOX = '/System/Library/PrivateFrameworks/RenderBox.framework/archive.metallib'
NRF = '/System/Library/VideoProcessors/NRFV1.bundle/binaryArchive.g12p'
LIB = ' lib=/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib '
RENDER = (
    f'R ok=1 label=- samples=1 topo=0 ds=260/260 c0=80:15:0:1:0:0:1:0:0 | v uber_vertex{LIB} | f uber_fragment{LIB}'
    '#0:36:000000000000000050f8010071000000 | '
)
NRF_LIB = 'lib=/System/Library/VideoProcessors/NRFV1.bundle/default.metallib'
COMPUTE = f'C ok=1 form=d opt=0 max=256 tgm=1 label=- | k RemixDenoise {NRF_LIB} #0:53:01 | '
KERNEL = f'C ok=1 form=d opt=0 max=0 tgm=0 label=- | k fill {NRF_LIB}  | '


def row(kind: str, path: str, family: str, line: str) -> str:
    return '\t'.join((kind, path.split('/')[-2], path, family, line))


class ArchivesTest(unittest.TestCase):
    def test_render_rows_take_the_runtimes_state_key(self):
        declared, kinds = archives.declared(row('render', RENDERBOX, 'device', RENDER))
        key = (
            'samples=1 topo=0 ds=260/260 c0=80:15:0:1:0:0:1:0:0  | v uber_vertex  | f uber_fragment'
            ' #0:36:000000000000000050f8010071000000 | l RenderBox'
        )
        self.assertEqual(declared, {RENDERBOX: {packfile.fnv(key.encode())}})
        self.assertEqual(kinds['render'], 1)

    def test_compute_rows_take_their_archive_key(self):
        # raw_compute_key.h computeArchiveKey: the library function, its constants, the descriptor's thread limits
        self.assertEqual(archives.compute_key(COMPUTE), 'k RemixDenoise #0:53:01 | max=256 tgm=1')
        self.assertEqual(archives.compute_key(KERNEL), 'k fill | max=0 tgm=0')
        # Core Image's archived kernels name no library: a creation's key names none either
        stitched = 'C ok=1 form=d opt=0 max=0 tgm=0 label=- | k ci_affine_clampRect  | '
        self.assertEqual(archives.compute_key(stitched), 'k ci_affine_clampRect | max=0 tgm=0')

    def test_another_familys_and_tile_rows_are_left_out(self):
        inventory = '\n'.join(
            [
                row('render', RENDERBOX, 'other', RENDER),
                row('tile', NRF, 'device', KERNEL),
                row('compute', NRF, 'device', KERNEL),
            ]
        )
        declared, kinds = archives.declared(inventory)
        self.assertEqual(list(declared), [NRF])
        self.assertEqual(dict(kinds), {'compute': 1})

    def test_a_held_kernel_is_its_key_up_to_its_shape(self):
        key = 'ab' * 32 + ' | #0:53:01 | max=0 tgm=0 | tg=8,8,1 | tgmem= | tex= | smp='
        self.assertEqual(
            archives.held_kernels([key]), {packfile.fnv(('ab' * 32 + ' | #0:53:01 | max=0 tgm=0').encode())}
        )

    def test_the_section_lists_held_kernels_first_then_archives_by_path_hash(self):
        data = archives.section({RENDERBOX: {9, 3}, NRF: {5}}, {11})
        count, keys = struct.unpack_from('<II', data)
        self.assertEqual((count, keys), (3, 4))
        heads = [struct.unpack_from('<QII', data, 8 + 16 * i) for i in range(count)]
        self.assertEqual(heads[0], (archives.HELD_KERNELS, 0, 1))
        self.assertEqual([h[0] for h in heads[1:]], sorted(packfile.fnv(p.encode()) for p in (RENDERBOX, NRF)))
        values = struct.unpack_from('<4Q', data, 8 + 16 * count)
        for _path_hash, first, n in heads:
            self.assertEqual(list(values[first : first + n]), sorted(values[first : first + n]))
        self.assertEqual(len(data), 8 + 16 * 3 + 8 * 4)


if __name__ == '__main__':
    unittest.main()
