import json
import struct
from dataclasses import dataclass, field

MAGIC = b'GM20BPK1'
# 2: the header's stack key (rawpack.py --stack) after the section table; 3: barrier-form records; 4: programs' shader
# data and constant buffer types; 6: compute kernels (runtime/raw_pack.h PackKernel); 8: a texture3d entry's Metal
# index; 9: split fragment programs (PackSplit)
VERSION = 9
SECTIONS = 10  # runtime/raw_pack.h PackSectionCount
KERNEL_ENTRIES = 16  # PackKernel.entry's length
PROGRAM_CBUFS = 11  # PackProgram.cbufType's length (runtime/raw_pack.h)


# PackSplit (runtime/raw_pack.h)
PACK_SPLIT = struct.Struct('<IHHHHBBBB8s64I')


def fnv(b: bytes, h: int = 0xCBF29CE484222325) -> int:
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# raw_pack.h kPackEntries: the set-0 entries a PackPipeline holds
PACK_ENTRIES = 32


def entry_bytes(entries: list[dict]) -> bytearray:
    if len(entries) > PACK_ENTRIES:
        raise ValueError(f'{len(entries)} set-0 entries: a pipeline holds {PACK_ENTRIES}')
    ents = bytearray()
    for e in entries:
        if e['kind'] == 'color_table':  # raw_pack.h EntryColorTable
            ents += struct.pack('<HBBBBBB', e['off'], 5, 2, 0, 0, 0, 0)
        elif e['kind'] == 'ubo':
            ents += struct.pack(
                '<HBBBBH',
                e['off'],
                1,
                1 if e['stage'] == 'v' else 2,
                e['slot'],
                buffer_kind(e),
                e.get('window', 256) // 16,
            )
        else:
            tk = e['tex']
            sk = e['smp']
            tkind = 0 if tk is None else {'slot': 2, 'fetch': 3, 'tex3d': 4}[tk[0]]
            skind = 0 if sk is None else (1 if sk[0] == 'slot' else 2)
            ents += struct.pack(
                '<HBBBBBB', e['off'], tkind, 2, 0 if tk is None else tk[1], skind, 0 if sk is None else sk[1], 0
            )
    ents += bytes(8 * (PACK_ENTRIES - len(entries)))
    return ents


def buffer_kind(e: dict) -> int:
    """Give a buffer entry's samplerKind byte (raw_pack.h bufferStorage, bufferResidualWord)."""
    residual = e.get('residual')
    word = 0 if residual is None else ((residual - 0x700) // 4 + 1) << 2
    return word | (1 if e.get('ssbo') else 0)


def push_bytes(push: list[tuple[int, int]]) -> bytes:
    mask, words = 0, [0] * 32
    for o, val in push:
        mask |= 1 << ((o - 0x700) // 4)
        words[(o - 0x700) // 4] = val
    return struct.pack('<I32I', mask, *words)


def key_hash(rec: dict) -> int:
    # the raw path's own states (probe.mm PROBE_OWN_LIB) by the name the runtime asks for (raw_gpu.cpp ycbcrPipeline and
    # standInPipeline, runtime/raw_xr.h xrConversion, runtime/raw_samples.h resolveState, runtime/raw_mip.h mipState)
    parts = rec['key'].split(' | ')
    pair = f'{parts[1].rstrip()} | {parts[2]}' if len(parts) == 3 else ''
    for family, vertex, fragments in (
        ('ycbcr', 'ycbcr_vert', ('converted_frag', 'srgb_frag', 'codes_frag', 'codes10_frag')),
        ('xr', 'xr_vert', ('bgr10_frag', 'bgra10_frag', 'planes_frag')),
        ('two_plane', 'two_plane_cover', ('load', 'store')),
        (
            'resolve',
            'resolve_vert',
            ('rgba8_frag', 'rgba8_srgb_frag', 'bgra8_frag', 'bgra8_srgb_frag', 'rg11b10f_frag', 'rgba16f_frag'),
        ),
        (
            'mip',
            'mip_vert',
            tuple(
                f'{name}_frag'
                for name in (
                    'a8',
                    'r16',
                    'r16f',
                    'b5g6r5',
                    'bgr5a1',
                    'r32f',
                    'rg16',
                    'rg16f',
                    'rgba8_srgb',
                    'bgra8_srgb',
                    'rgb10a2',
                    'rg11b10f',
                    'bgr10a2',
                    'rg32f',
                    'rgba16',
                    'rgba32f',
                )
            ),
        ),
    ):
        for fragment in fragments:
            name = f'{family}_{fragment}'
            if pair == f'v {vertex} | f {name}':
                return fnv(f'raw.{family}.{name}'.encode())
    return fnv(rec['key'].encode())


def pipeline_bytes(rec: dict, key_offset: int, blob_off: int) -> bytes:
    vb = rec['vbs'][0] if rec['vbs'] else {'slot': 0xFF, 'stride': 0}
    keyhash = key_hash(rec)
    flags = (1 if rec['fetch'] else 0) | (2 if rec.get('form') == 'barrier' else 0)  # PackPipelineFlags
    head = struct.pack(
        '<QIHHIIBBBBHHI',
        keyhash,
        key_offset,
        rec['vs'],
        rec['fs'],
        blob_off,
        len(rec['blob']) * 2,
        len(rec['entries']),
        len(rec['push']),
        vb['slot'],
        0,
        vb['stride'],
        rec['set_size'],
        flags,
    )
    return head + entry_bytes(rec['entries']) + push_bytes(rec['push'])


def kernel_window(e: dict) -> int:
    """Give a kernel entry's window word: a buffer's window bytes, its residual word + 1 in the top byte (0: none)."""
    residual = e.get('residual')
    return e['window'] | (residual + 1) << 24 if residual is not None else e['window']


def kernel_bytes(k: dict, key_offset: int) -> bytes:
    """Give a compute kernel's PackKernel: its key, program, QMD template, set size, push words and set-0 entries."""
    mask, words = 0, [0] * 32
    for w, v in k['push'].items():
        mask |= 1 << w
        words[w] = v
    entries = b''.join(struct.pack('<HBBI', e['off'], e['kind'], e['slot'], kernel_window(e)) for e in k['entries'])
    entries += bytes(8 * (KERNEL_ENTRIES - len(k['entries'])))
    return (
        struct.pack('<QIHBb', fnv(k['key'].encode()), key_offset, k['program'], len(k['entries']), k['region'])
        + struct.pack('<64I', *k['qmd'])
        + struct.pack('<HHI', k['set_bytes'], 0, mask)
        + struct.pack('<32I', *words)
        + entries
    )


def program_name(program: dict) -> str:
    """Give the name nvdec gives a bound program: the FNV-1a of its header, code and shader data.

    The pack's ids hold its constant buffer types too (directlink.py program_id), which nvdec cannot see.
    """
    return f'{fnv(program["image"] + program["data"]):x}'


def place_graphics(
    pid: str, programs: dict[str, dict], heap: bytearray, prog_index: dict[str, int], prog_recs: list[dict]
) -> None:
    """Append a 3D program to the heap image as NVK uploads it pre-Turing: header at +0x30, code at +0x80."""
    if pid in prog_index:
        return
    pr = programs[pid]
    storage_id = pr.get('storage_id', pid)
    shared = next((p for p in prog_recs if p.get('storage_id') == storage_id), None)
    if shared:
        prog_index[pid] = len(prog_recs)
        prog_recs.append(dict(shared, id=pid))
        return
    off = len(heap)
    slot = bytearray(0x80 - 0x50) + pr['image'][:80]
    slot += pr['image'][80:]
    slot += bytes((-len(slot)) % 0x100)
    data_offset = len(slot) if pr['data'] else 0  # NAK's shader data where NVK uploads it: 256-aligned after the code
    slot += pr['data']
    heap += slot
    prog_index[pid] = len(prog_recs)
    prog_recs.append(
        {
            'id': pid,
            'storage_id': storage_id,
            'name': program_name(pr),
            'heap_offset': off,
            'program': off + 0x30,
            'bytes': len(slot),
            'code': len(pr['image']) - 80,
            'cbufs': pr['cbufs'],
            'data_offset': data_offset,
            'data_bytes': len(pr['data']),
        }
    )


def split_bytes(found: list[dict]) -> bytes:
    """Give the PackSplit section: the splits in whole-program order (raw_pack.cpp Pack::split searches it)."""
    return b''.join(
        PACK_SPLIT.pack(
            s['threshold'],
            s['whole'],
            s['fs'],
            s['prologue'],
            s['frontier_bytes'],
            s['frontier_cbuf'],
            len(s['ubo_cbufs']),
            s['shader_data_cbuf'],
            s['prologue_cbufs'],
            bytes(s['ubo_cbufs']) + bytes(8 - len(s['ubo_cbufs'])),
            *s['qmd'],
        )
        for s in found
    )


@dataclass
class Pack:
    pipes: list[dict]
    programs: list[dict]
    constexpr_tsc: list[list[int]]
    baseline: dict[int, int]
    init: list[tuple[int, int, int, int]]
    heap: bytearray
    kernels: list[dict]
    splits: list[dict] = field(default_factory=list)


def pipeline_sections(pipes: list[dict]) -> tuple[bytearray, list[int], bytearray]:
    strings = bytearray()

    def string(s: str) -> int:
        off = len(strings)
        strings.extend(s.encode() + b'\0')
        return off

    blob = []
    pipe_bin = bytearray()
    for rec in pipes:
        blob_off = len(blob)
        for m, v in rec['blob']:
            blob += [m, v]
        # the key's attachment and vertex fields (what the runtime reads of it: raw_gpu.cpp Device::pipeline), not the
        # functions' constants (some 1.5 KB a fixed-function state; pack.json has the whole key)
        pipe_bin += pipeline_bytes(rec, string(rec['key'].split(' | ')[0].rstrip()), blob_off)
    return pipe_bin, blob, strings


def pack_bytes(pack: Pack) -> tuple[bytes, list[int]]:
    pipe_bin, blob, strings = pipeline_sections(pack.pipes)
    kernel_bin = bytearray()
    for k in sorted(pack.kernels, key=lambda k: fnv(k['key'].encode())):
        kernel_bin += kernel_bytes(k, len(strings))
        strings.extend(k['key'].encode() + b'\0')
    prog_bin = bytearray()
    for pr in pack.programs:
        types = bytes(pr['cbufs']) + bytes(PROGRAM_CBUFS - len(pr['cbufs']))
        prog_bin += (
            struct.pack(
                '<IIIIIB',
                pr['heap_offset'],
                pr['program'],
                pr['bytes'],
                pr['data_offset'],
                pr['data_bytes'],
                len(pr['cbufs']),
            )
            + types
        )
    tsc_bin = b''.join(struct.pack('<8I', *w) for w in pack.constexpr_tsc)
    base_bin = b''.join(struct.pack('<II', m, v) for m, v in sorted(pack.baseline.items()))
    init_bin = b''.join(struct.pack('<HHII', sc, m, d, rc) for sc, m, d, rc in pack.init)
    blob_bin = struct.pack(f'<{len(blob)}I', *blob)
    sections = [
        bytes(prog_bin),
        bytes(pipe_bin),
        tsc_bin,
        base_bin,
        init_bin,
        blob_bin,
        bytes(strings),
        bytes(pack.heap),
        bytes(kernel_bin),
        split_bytes(pack.splits),
    ]
    assert len(sections) == SECTIONS
    header_size = 8 + 4 * 4 + 8 * 2 * len(sections)
    offs, at = [], (header_size + 255) // 256 * 256
    body = bytearray()
    for s in sections:
        pad = (-(at + len(body))) % 256
        body += bytes(pad)
        offs.append((at + len(body), len(s)))
        body += s
    hdr = MAGIC + struct.pack('<IIII', VERSION, len(pack.programs), len(pack.pipes), len(pack.constexpr_tsc))
    hdr += struct.pack(f'<{2 * len(sections)}Q', *[x for o in offs for x in o])
    return hdr + bytes((header_size + 255) // 256 * 256 - len(hdr)) + body, blob


def write_json(path: str, pack: Pack, varying: list[int]) -> None:
    corpus = fnv(''.join(sorted(r['key'] for r in pack.pipes)).encode())
    js = {
        'version': VERSION,
        'corpus_hash': f'{corpus:x}',
        'programs': pack.programs,
        'pipelines': pack.pipes,
        'constexpr_tsc': pack.constexpr_tsc,
        'baseline': {f'{m:x}': v for m, v in sorted(pack.baseline.items())},
        'varying': [f'{m:x}' for m in varying],
        'init_methods': len(pack.init),
        'kernels': pack.kernels,
        'splits': pack.splits,
    }
    with open(path, 'w') as pack_json:
        json.dump(
            js,
            pack_json,
            indent=1,
            default=lambda o: o.hex() if isinstance(o, (bytes, bytearray)) else str(o),
        )


def entry_kind(e: dict) -> str:
    # u: a buffer descriptor (the colour table's too), t: a handle whose sampler is unused
    return 'u' if e['kind'] in ('ubo', 'color_table') else ('h' if e.get('smp') else 't')


def name(program: dict) -> str:
    return program.get('name', program['id'])


def write_layouts(path: str, pack: Pack) -> None:
    # decoder table: pipeline set-0 layouts by program pair, so nvdec compares only the bytes a layout defines
    split = {s['whole']: s['fs'] for s in pack.splits}
    with open(path, 'w') as f:
        for rec in pack.pipes:
            slots = ' '.join(f'{e["off"]}:{entry_kind(e)}' for e in rec['entries'])
            vs = name(pack.programs[rec['vs']])
            for fs in (rec['fs'], split.get(rec['fs'])):
                if fs is not None:
                    f.write(f'{vs} {name(pack.programs[fs])} {rec["set_size"]:x} {slots}\n')
