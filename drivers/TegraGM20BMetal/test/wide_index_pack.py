"""Write a pack whose program indices pass 65,535, by the pack writer's own codec (raw/pack/packfile.py).

usage: wide_index_pack.py <out pack.bin>
raw_pack_test reads it back with the runtime's reader (raw/runtime/raw_pack.h): the two agree on every index field, and
on a program of more constant buffers than the 11 PackProgram once held.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raw', 'pack'))
import packfile

STATE_KEY = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v wide_vertex  | f wide_fragment'
KERNEL_KEY = 'kernel | wide_kernel'
VERTEX, FRAGMENT, VARIANT, KERNEL, SPLIT_FRAGMENT, PROLOGUE = 70000, 70001, 70002, 70003, 70004, 70005


# VectorKit's DaVinci ground_base_vertex on 21H461 reads the root table and 11 uniform buffers: 12 constant buffers
WIDE_CBUFS = [1, *(5 | ubo << 3 for ubo in range(11))]


def program(cbufs: list[int] | None = None) -> dict:
    return {
        'heap_offset': 0,
        'program': 0x30,
        'bytes': 0x100,
        'data_offset': 0,
        'data_bytes': 0,
        'cbufs': cbufs or [1],
        'gprs': 40,
        'bound_textures': 2,
    }


def pack() -> packfile.Pack:
    state = {
        'key': STATE_KEY,
        'vs': VERTEX,
        'fs': FRAGMENT,
        'variants': [{'stage': 1, 'assume': 1, 'program': VARIANT}],
        'blob': [],
        'entries': [],
        'push': [],
        'vbs': [],
        'set_size': 0,
        'fetch': False,
    }
    kernel = {
        'key': KERNEL_KEY,
        'program': KERNEL,
        'entries': [],
        'region': -1,
        'qmd': [0] * 64,
        'set_bytes': 0,
        'push': {},
    }
    split = {
        'threshold': 40,
        'whole': FRAGMENT,
        'fs': SPLIT_FRAGMENT,
        'prologue': PROLOGUE,
        'frontier_bytes': 16,
        'frontier_cbuf': 2,
        'ubo_cbufs': [],
        'shader_data_cbuf': 1,
        'prologue_cbufs': 2,
        'qmd': [0] * 64,
    }
    return packfile.Pack(
        pipes=[state],
        programs=[program(), program(WIDE_CBUFS)],
        constexpr_tsc=[],
        baseline={},
        init=[],
        heap=bytearray(0x100),
        kernels=[kernel],
        splits=[split],
    )


def main() -> None:
    data, _blob = packfile.pack_bytes(pack())
    with open(sys.argv[1], 'wb') as out:
        out.write(data)


if __name__ == '__main__':
    main()
