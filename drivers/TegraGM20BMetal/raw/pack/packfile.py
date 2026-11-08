import json
import re
import struct
from dataclasses import dataclass

MAGIC = b'GM20BPK1'
# 2: the header's stack key (rawpack.py --stack) after the section table; 3: barrier-form records; 4: programs' shader
# data and constant buffer types
VERSION = 5
PROGRAM_CBUFS = 11  # PackProgram.cbufType's length (runtime/raw_pack.h)


def fnv(b: bytes, h: int = 0xCBF29CE484222325) -> int:
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def entry_bytes(entries: list[dict]) -> bytearray:
    ents = bytearray()
    for e in entries:
        if e['kind'] == 'ubo':
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
            tkind = 0 if tk is None else (2 if tk[0] == 'slot' else 3)
            skind = 0 if sk is None else (1 if sk[0] == 'slot' else 2)
            ents += struct.pack(
                '<HBBBBBB', e['off'], tkind, 2, 0 if tk is None else tk[1], skind, 0 if sk is None else sk[1], 0
            )
    ents += bytes(8 * (16 - len(entries)))
    return ents


def buffer_kind(e: dict) -> int:
    """Give a buffer entry's samplerKind byte (raw_pack.h bufferStorage, bufferResidualWord)."""
    if e.get('ssbo'):
        return 1
    residual = e.get('residual')
    return 0 if residual is None else ((residual - 0x700) // 4 + 1) << 2


def push_bytes(push: list[tuple[int, int]]) -> bytes:
    mask, words = 0, [0] * 32
    for o, val in push:
        mask |= 1 << ((o - 0x700) // 4)
        words[(o - 0x700) // 4] = val
    return struct.pack('<I32I', mask, *words)


def key_hash(rec: dict) -> int:
    # the raw path's own states (probe.mm PROBE_OWN_LIB) by the name the runtime asks for (raw_gpu.cpp ycbcrPipeline and
    # standInPipeline, runtime/raw_xr.h xrConversion)
    own = re.search(r'\| f ((ycbcr|xr|two_plane)_\w+)', rec['key'])
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


@dataclass
class Pack:
    pipes: list[dict]
    programs: list[dict]
    constexpr_tsc: list[list[int]]
    baseline: dict[int, int]
    init: list[tuple[int, int, int, int]]
    heap: bytearray


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
    ]
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
    }
    with open(path, 'w') as pack_json:
        json.dump(
            js,
            pack_json,
            indent=1,
            default=lambda o: o.hex() if isinstance(o, (bytes, bytearray)) else str(o),
        )


def entry_kind(e: dict) -> str:
    return 'u' if e['kind'] == 'ubo' else ('h' if e.get('smp') else 't')  # t: a handle whose sampler is unused


def write_layouts(path: str, pack: Pack) -> None:
    # decoder table: pipeline set-0 layouts by program pair, so nvdec compares only the bytes a layout defines
    with open(path, 'w') as f:
        for rec in pack.pipes:
            vs, fs = pack.programs[rec['vs']]['id'], pack.programs[rec['fs']]['id']
            slots = ' '.join(f'{e["off"]}:{entry_kind(e)}' for e in rec['entries'])
            f.write(f'{vs} {fs} {rec["set_size"]:x} {slots}\n')
