import json
import re
import struct
from dataclasses import dataclass

MAGIC = b'GM20BPK1'
# 2: the header's stack key (rawpack.py --stack) after the section table; 3: barrier-form records; 4: programs' shader
# data and constant buffer types; 6: compute kernels (runtime/raw_pack.h PackKernel); 8: a texture3d entry's Metal index
VERSION = 8
SECTIONS = 9  # runtime/raw_pack.h PackSectionCount
KERNEL_ENTRIES = 16  # PackKernel.entry's length
PROGRAM_CBUFS = 11  # PackProgram.cbufType's length (runtime/raw_pack.h)


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
    # standInPipeline, runtime/raw_xr.h xrConversion, runtime/raw_samples.h resolveState)
    own = re.search(r'\| f ((ycbcr|xr|two_plane|resolve)_\w+)', rec['key'])
    return fnv(f'raw.{own.group(2)}.{own.group(1)}'.encode() if own else rec['key'].encode())


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


@dataclass
class Pack:
    pipes: list[dict]
    programs: list[dict]
    constexpr_tsc: list[list[int]]
    baseline: dict[int, int]
    init: list[tuple[int, int, int, int]]
    heap: bytearray
    kernels: list[dict]


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


def write_layouts(path: str, pack: Pack) -> None:
    # decoder table: pipeline set-0 layouts by program pair, so nvdec compares only the bytes a layout defines
    with open(path, 'w') as f:
        for rec in pack.pipes:
            vs, fs = pack.programs[rec['vs']]['id'], pack.programs[rec['fs']]['id']
            slots = ' '.join(f'{e["off"]}:{entry_kind(e)}' for e in rec['entries'])
            f.write(f'{vs} {fs} {rec["set_size"]:x} {slots}\n')
