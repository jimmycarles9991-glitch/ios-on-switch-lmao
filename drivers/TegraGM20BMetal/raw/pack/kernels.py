"""The compute kernels directpack.mm made directly, for directlink.py.

A DKRN record holds its PackKernel with program index 0, its program (code, shader data, constant buffer types) and
sampler headers, which its sampler entries name as CONSTEXPR | n. The pack numbers those headers among its constexpr
samplers, and a buffer entry's window keeps its residual word + 1 in its top byte (runtime/raw_pack.h).
"""

import struct

import packfile

CONSTEXPR = 0x80  # PackKernelEntry.slot of a constexpr sampler: its index in the pack's constexpr samplers
BUFFER, TEXTURE, SAMPLER = 0, 1, 2  # runtime/raw_pack.h PackKernelEntryKind
HEAD = struct.Struct('<QIHBb')
ENTRY = struct.Struct('<HBBI')
ENTRIES = 16
KERNEL_BYTES = HEAD.size + 64 * 4 + 8 + 32 * 4 + ENTRIES * ENTRY.size


def entry(fields: tuple[int, int, int, int], headers: list[list[int]], constexpr_tsc: list[list[int]]) -> dict:
    """Give a record's set-0 entry as packfile.py takes it, its sampler numbered among the pack's."""
    off, kind, slot, window = fields
    if kind == SAMPLER:
        header = headers[slot & (CONSTEXPR - 1)]
        if header not in constexpr_tsc:
            constexpr_tsc.append(header)
        return {'off': off, 'kind': kind, 'slot': CONSTEXPR | constexpr_tsc.index(header), 'window': 0}
    if kind == BUFFER:
        residual = (window >> 24) - 1
        return {
            'off': off,
            'kind': kind,
            'slot': slot,
            'window': window & 0xFFFFFF,
            'residual': residual if residual >= 0 else None,
        }
    return {'off': off, 'kind': kind, 'slot': slot, 'window': window}


def kernel(rec: dict, constexpr_tsc: list[list[int]]) -> dict:
    """Give a made record's kernel as packfile.py takes it, its program as directlink.py names one (its 'prog')."""
    k = rec['kernel']
    _hash, _key, _program, count, region = HEAD.unpack_from(k)
    qmd = list(struct.unpack_from('<64I', k, HEAD.size))
    set_bytes, _pad, mask = struct.unpack_from('<HHI', k, HEAD.size + 256)
    words = struct.unpack_from('<32I', k, HEAD.size + 264)
    at = HEAD.size + 264 + 128
    entries = [entry(ENTRY.unpack_from(k, at + ENTRY.size * i), rec['samplers'], constexpr_tsc) for i in range(count)]
    return {
        'key': rec['key'],
        'region': region,
        'set_bytes': set_bytes,
        'qmd': qmd,
        'push': {w: words[w] for w in range(32) if mask >> w & 1},
        'entries': entries,
        'prog': rec['program'],
    }


def from_records(records: list[dict], constexpr_tsc: list[list[int]]) -> tuple[list[dict], list[tuple[str, str]]]:
    """Give the made kernels, each key once in the buckets' order, and the shapes left out with why."""
    kernels, skipped, seen = [], [], set()
    for rec in records:
        if rec['key'] in seen:
            continue
        seen.add(rec['key'])
        if rec['outcome']:
            skipped.append((rec['key'], rec['why']))
            continue
        kernels.append(kernel(rec, constexpr_tsc))
    return kernels, skipped


def lay_out(kernels: list[dict], programs: dict[str, dict], heap: bytearray, prog_index: dict, prog_recs: list) -> None:
    """Append each kernel's program to the heap image and set each kernel's program index.

    Its code starts its slot (no shader header: NVK's compute upload), its shader data 256-aligned after.
    """
    for k in kernels:
        pid = k.pop('prog')
        if pid not in prog_index:
            pr = programs[pid]
            heap += bytes((-len(heap)) % 0x100)
            off = len(heap)
            slot = bytearray(pr['image'][80:])
            slot += bytes((-len(slot)) % 0x100)
            data_offset = len(slot) if pr['data'] else 0
            slot += pr['data']
            heap += slot
            prog_index[pid] = len(prog_recs)
            prog_recs.append(
                {
                    'id': pid,
                    'name': packfile.program_name(pr),
                    'heap_offset': off,
                    'program': off,
                    'bytes': len(slot),
                    'code': len(pr['image']) - 80,
                    'cbufs': pr['cbufs'],
                    'data_offset': data_offset,
                    'data_bytes': len(pr['data']),
                }
            )
        k['program'] = prog_index[pid]
