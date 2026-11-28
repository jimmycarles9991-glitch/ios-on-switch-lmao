"""Generate a two-tier pack fixture (v34 and later) exercising every pre-built state-segment path.

usage: pack_v34_fixture.py <out pack.bin>
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pack'))
import packfile
import segments

PLAIN_KEY = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v v  | f f'
BLEND_INTERLOCK_KEY = 'samples=2 topo=0 ds=0/0 c0=80:15:1:4:5:0:4:5:0  | v blend_v  | f blend_f'
NO_PIXEL_RAST0_KEY = 'samples=1 topo=0 ds=0/0 rast=0  | v nopix_v  | f -'
TESS_KEY = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v tess_v  | f tess_f'
FETCH_KEY = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v fetch_v  | f fetch_f'
MATRIX_KEY = 'samples=1 topo=0 ds=0/0 c0=115:15:0:1:0:0:1:0:0  | v mat_v  | f mat_f'
VARIANTS_KEY = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v var_v  | f var_f'
STRIDE_TWIN_KEY = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v stride_twin_v  | f stride_twin_f'
STRIDE_MAIN_KEY = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v stride_v  | f stride_f'
PASS_WRITE_KEY = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v passw_v  | f passw_f'
GUEST_BASE_KEY = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v guest_v  | f guest_f'
GUEST_TWIN_KEY = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v guest_twin_v  | f guest_twin_f'
TIER1_KEY = 'samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v cold_v  | f cold_f'

SET_PROGRAM_REGION_A = 0x1608
SET_PROGRAM_REGION_B = 0x160C
RELOC_HEAP_HI = 1
RELOC_HEAP_LO = 2


def add_program(
    heap: bytearray,
    programs: list[dict],
    *,
    data_bytes: int = 0,
    cbufs: list[int] | None = None,
    gprs: int = 16,
    bound_textures: int = 0,
    slm_per_thread: int = 0,
    crs: int = 0,
) -> int:
    off = len(heap)
    code_bytes = 0x100
    slot = bytearray(code_bytes + data_bytes)
    # SPH at +0x30: word 1 = per-thread local memory, word 3 = CRS spill size.
    slot[0x34:0x38] = slm_per_thread.to_bytes(4, 'little')
    slot[0x3C:0x40] = crs.to_bytes(4, 'little')
    if data_bytes:
        for i in range(data_bytes):
            slot[code_bytes + i] = (i * 5 + len(programs)) & 0xFF
    heap.extend(slot)
    idx = len(programs)
    programs.append(
        {
            'heap_offset': off,
            'program': off + 0x30,
            'bytes': len(slot),
            'data_offset': code_bytes if data_bytes else 0,
            'data_bytes': data_bytes,
            'cbufs': cbufs if cbufs is not None else [1],
            'gprs': gprs,
            'bound_textures': bound_textures,
            'far_ubos': [],
            'facts': 0,
        }
    )
    return idx


def base_record(key: str, vs: int, fs: int, blob: list[tuple[int, int]], **extra) -> dict:
    rec = {
        'tier': 0,
        'key': key,
        'vs': vs,
        'fs': fs,
        'blob': blob,
        'entries': [],
        'push': [],
        'vbs': [],
        'set_size': 0,
        'fetch': False,
    }
    rec.update(extra)
    return rec


def build_fixture() -> packfile.Pack:
    heap = bytearray()
    programs: list[dict] = []

    vs_data = add_program(heap, programs, data_bytes=0x100, cbufs=[1, 2], gprs=16)
    fs_data = add_program(heap, programs, data_bytes=0x100, cbufs=[1, 2, 3], gprs=24, bound_textures=3)
    fs_plain = add_program(heap, programs, cbufs=[1, 3], gprs=20)
    fs_slm = add_program(heap, programs, cbufs=[1, 3], gprs=28, slm_per_thread=64, crs=512)
    fs_turned = add_program(heap, programs, cbufs=[1, 3], gprs=18)
    tess_ctrl = add_program(heap, programs, data_bytes=0x100, cbufs=[1, 2], gprs=14)
    tess_eval = add_program(heap, programs, cbufs=[1], gprs=12)
    split_fs = add_program(heap, programs, cbufs=[1, 3], gprs=16)
    split_prologue = add_program(heap, programs, cbufs=[1], gprs=0)
    tier0_progs = len(programs)

    # Pad tier 0 heap to TIER_ALIGN so tier-1 programs sit cleanly past the pack window.
    heap.extend(bytes(packfile.TIER_ALIGN - len(heap)))
    tier0_heap = len(heap)

    t1_vs = add_program(heap, programs, data_bytes=0x100, cbufs=[1, 2], gprs=16)
    t1_fs = add_program(heap, programs, cbufs=[1, 3], gprs=22)
    # Pad tier-1 heap past kProgramPrefetchBytes (0x1000) so tier-1 programs run in place via ProgramPages.
    heap.extend(bytes(packfile.TIER_ALIGN - (len(heap) - tier0_heap)))

    iterated = [
        {
            'name': 'pack_hit_test_it',
            'words': [0x00000011, 0x00000022],
            'enable': 3,
            'constants': {0: (0x3C00, 0x3800, 0x3400), 1: (0x3000, 0x2C00, 0x2800)},
            'alpha_source': 0x4001,
            'alpha_dest': 0x4303,
        },
        {
            'name': 'vcm',
            'words': [],
            'enable': 1,
            'constants': {},
            'uniform_slot': 0,
            'uniform_offset': 0,
            'uniform_words': 10,
            'alpha_source': 0x4001,
            'alpha_dest': 0x4303,
        },
    ]

    stride_twin_hash = packfile.fnv(STRIDE_TWIN_KEY.encode())
    guest_twin_hash = packfile.fnv(GUEST_TWIN_KEY.encode())

    pipes = [
        # Plain tier-0 QuartzCore state with CbufShaderData (FLAG_RESELECT_ROOT) plus owned + swizzle methods dropped.
        base_record(
            PLAIN_KEY,
            vs_data,
            fs_data,
            [
                (0x0790, 0xDEADBEEF),  # ownedMethod: SET_SHADER_LOCAL_MEMORY_A (dropped)
                (0x0A18, 0x00001234),  # swizzleMethod: SET_VIEWPORT_COORDINATE_SWIZZLE(0) (dropped)
                (0x1A00, 0x1111),
                (0x193C, 0x00001819),  # SET_VIEWPORT_CLIP_CONTROL
                (0x1364, 0x00000001),
                (0x1368, 0x00000002),
                (0x136C, 0x00000003),
            ],
        ),
        # 2x MSAA raster-ordered state triggering legalInterlockControl (2 -> 1) and blendKill (NOOP_SOURCE_RGBA_0000).
        base_record(
            BLEND_INTERLOCK_KEY,
            vs_data,
            fs_plain,
            [
                (0x1A00, 0x1111),
                (0x1224, 2),  # PIXEL_SHADER_INTERLOCK_CONTROL
                (0x0754, 0x0012),  # HYBRID_ANTI_ALIAS_CONTROL: 2 passes
                (0x1360, 1),  # SET_BLEND(0)
                (0x1E04, 0x8006),
                (0x1E08, 0x4001),
                (0x1E0C, 0x4303),
                (0x1E10, 0x8006),
                (0x1E14, 0x4001),
                (0x1E18, 0x4303),
            ],
            raster_ordered=True,
        ),
        # No-pixel + rast=0 state exercising stageSwitches and stageSwitchesRestore.
        base_record(
            NO_PIXEL_RAST0_KEY,
            vs_data,
            packfile.NO_PROGRAM,
            [
                (0x1364, 0x00000010),
                (0x1368, 0x00000011),
                (0x136C, 0x00000012),
                (0x12EC, 0x00000000),
            ],
        ),
        # Tessellation state with control/eval programs and tess CbufShaderData.
        base_record(
            TESS_KEY,
            vs_data,
            fs_plain,
            [
                (0x1A00, 0x1111),
                (0x1364, 0x00000020),
                (0x1368, 0x00000021),
                (0x136C, 0x00000022),
            ],
            runtime_head=bytes([1, 3, 0, 0, 0, 0, 0]),
            tess=(tess_ctrl, tess_eval),
        ),
        # Storage + barrier + fetch-free iterated blend (fixed program with constants + BGRA8 saturate).
        base_record(
            FETCH_KEY,
            vs_data,
            fs_plain,
            [
                (0x1A00, 0),
                (0x1224, 2),
                (0x0754, 1),
                (0x1364, 0x00000030),
                (0x1368, 0x00000031),
                (0x136C, 0x00000032),
            ],
            fetch=True,
            entries=[{'off': 0, 'kind': 'handle', 'stage': 'f', 'tex': ('fetch', 0), 'smp': None}],
            storage_mask=1,
            storage_write_mask=1,
            storage_samples=1,
            storage_tic=[0x54E24908, 0, 0, 0, 0, 0, 0, 0],
        ),
        base_record(
            FETCH_KEY,
            vs_data,
            fs_plain,
            [
                (0x1A00, 0x1111),
                (0x1364, 0x00000033),
                (0x1368, 0x00000034),
                (0x136C, 0x00000035),
            ],
            form='barrier',
            fetch=True,
            entries=[{'off': 0, 'kind': 'handle', 'stage': 'f', 'tex': ('fetch', 0), 'smp': None}],
        ),
        base_record(
            FETCH_KEY,
            vs_data,
            fs_plain,
            [
                (0x1A00, 0x1111),
                (0x1364, 0x00000036),
                (0x1368, 0x00000037),
            ],
            form='free',
            fetch_rung=[3, 0, 0, 0, 0, 0, 0, 0],
            iterated=1,
        ),
        # Fetch-free iterated blend colour matrix (vcm) on RGBA16Float (saturate=false).
        base_record(
            MATRIX_KEY,
            vs_data,
            fs_plain,
            [
                (0x1A00, 0x1111),
                (0x1364, 0x00000040),
                (0x1368, 0x00000041),
            ],
            form='free',
            fetch_rung=[3, 0, 0, 0, 0, 0, 0, 0],
            iterated=2,
        ),
        # Variants (AssumeTurned + AssumeUniformAligned with 2560 B/warp SLM) + PackSplit on fs_plain.
        base_record(
            VARIANTS_KEY,
            vs_data,
            fs_plain,
            [
                (0x1A00, 0x1111),
                (0x1364, 0x00000050),
                (0x1368, 0x00000051),
                (0x136C, 0x00000052),
            ],
            variants=[
                {'stage': 1, 'assume': 1 << 26, 'program': fs_turned, 'memory': 0},
                {'stage': 1, 'assume': 1, 'program': fs_slm, 'memory': 0},
            ],
        ),
        # Stride twin target + main record with strideTwin and PipelineVariantRecord (" | tests=2").
        base_record(
            STRIDE_TWIN_KEY,
            vs_data,
            fs_plain,
            [
                (0x1A00, 0x1111),
                (0x1364, 0x00000060),
                (0x1368, 0x00000061),
                (0x136C, 0x00000062),
            ],
        ),
        base_record(
            STRIDE_MAIN_KEY,
            vs_data,
            fs_plain,
            [
                (0x1A00, 0x1111),
                (0x1364, 0x00000063),
                (0x1368, 0x00000064),
                (0x136C, 0x00000065),
            ],
            stride_twin=stride_twin_hash,
        ),
        base_record(
            STRIDE_MAIN_KEY + ' | tests=2',
            vs_data,
            fs_plain,
            [
                (0x1A00, 0x1111),
                (0x1364, 0x00000066),
                (0x1368, 0x00000067),
                (0x136C, 0x00000068),
            ],
        ),
        # Pass-write register in blob (keptAcrossPasses = False).
        base_record(
            PASS_WRITE_KEY,
            vs_data,
            fs_plain,
            [
                (0x1A00, 0x1111),
                (0x0DC0, 1),  # SET_POLY_OFFSET_POINT
                (0x1364, 0x00000070),
                (0x1368, 0x00000071),
                (0x136C, 0x00000072),
            ],
        ),
        # Its programs read vertex buffer 5's and fragment buffer 2's Metal addresses (push words 6-7 and 2-3); its program
        # variants are of the same record, its stride twin a record of its own that reads vertex buffer 1's (words 8-9).
        base_record(
            GUEST_BASE_KEY,
            vs_data,
            fs_plain,
            [
                (0x1A00, 0x1111),
                (0x1364, 0x00000074),
                (0x1368, 0x00000075),
                (0x136C, 0x00000076),
            ],
            guest_base_word=[7, 3],
            guest_base_slot=[5, packfile.GUEST_BASE_FRAGMENT | 2],
            variants=[
                {'stage': 1, 'assume': 1 << 26, 'program': fs_turned, 'memory': 0},
                {'stage': 1, 'assume': 1, 'program': fs_slm, 'memory': 0},
            ],
            stride_twin=guest_twin_hash,
        ),
        base_record(
            GUEST_TWIN_KEY,
            vs_data,
            fs_plain,
            [
                (0x1A00, 0x1111),
                (0x1364, 0x00000077),
                (0x1368, 0x00000078),
                (0x136C, 0x00000079),
            ],
            guest_base_word=[9],
            guest_base_slot=[1],
        ),
        # A tier-1 state's variant record, which its draws' first select of " | tests=2" places on the placer.
        base_record(
            TIER1_KEY + ' | tests=2',
            t1_vs,
            t1_fs,
            [
                (0x1A00, 0x1111),
                (0x1364, 0x00000083),
                (0x1368, 0x00000084),
                (0x136C, 0x00000085),
            ],
            tier=1,
        ),
        # Tier-1 cold record past tier0_heap.
        base_record(
            TIER1_KEY,
            t1_vs,
            t1_fs,
            [
                (0x1A00, 0x1111),
                (0x1364, 0x00000080),
                (0x1368, 0x00000081),
                (0x136C, 0x00000082),
            ],
            tier=1,
        ),
    ]

    pipes.sort(key=lambda r: (r['tier'], packfile.key_hash(r)))

    fast_keys: dict[int, int] = {}
    for rec in pipes:
        h = packfile.key_hash(rec)
        fast_keys[h] = h

    splits = [
        {
            'threshold': 40,
            'whole': fs_plain,
            'fs': split_fs,
            'prologue': split_prologue,
            'frontier_bytes': 32,
            'frontier_cbuf': 2,
            'ubo_cbufs': [],
            'shader_data_cbuf': 0,
            'prologue_cbufs': 1,
            'qmd': [0] * 64,
        }
    ]

    init = [
        (0, SET_PROGRAM_REGION_A, 0, RELOC_HEAP_HI),
        (0, SET_PROGRAM_REGION_B, 0, RELOC_HEAP_LO),
    ]

    pack = packfile.Pack(
        pipes=pipes,
        programs=programs,
        constexpr_tsc=[],
        baseline={0x1A00: 0x1111},
        init=init,
        heap=heap,
        kernels=[],
        splits=splits,
        iterated=iterated,
        fast_keys=fast_keys,
        tier0_programs=tier0_progs,
        tier0_heap=tier0_heap,
    )
    segments.build_tier0_segments(pack)
    return pack


def main() -> None:
    data, _blob = packfile.pack_bytes(build_fixture())
    with open(sys.argv[1], 'wb') as out:
        out.write(data)


if __name__ == '__main__':
    main()
