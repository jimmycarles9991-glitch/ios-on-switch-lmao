"""The compute kernels directpack.mm made directly, for directlink.py.

A DKRN record holds its PackKernel with program index 0, its program (code, shader data, constant buffer types) and
sampler headers, which its sampler entries name as CONSTEXPR | n. The pack numbers those headers among its constexpr
samplers, and a buffer entry's window keeps its residual word + 1 in its top byte (runtime/raw_pack.h).
"""

import struct

import packfile

CONSTEXPR = 0x80  # PackKernelEntry.slot of a constexpr sampler: its index in the pack's constexpr samplers
BUFFER, TEXTURE, SAMPLER = 0, 1, 2  # runtime/raw_pack.h PackKernelEntryKind
HEAD = struct.Struct('<QIIBb')
ENTRY = struct.Struct('<HBBI')
ENTRIES = packfile.KERNEL_ENTRIES
COUNTS = struct.Struct('<HQI')  # setBytes, readOnlyEntries, pushMask
KERNEL_BYTES = (
    HEAD.size
    + 64 * 4
    + COUNTS.size
    + 32 * 4
    + ENTRIES * ENTRY.size
    + packfile.KERNEL_V20.size
    + packfile.KERNEL_V21.size
)


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
    set_bytes, read_only, mask = COUNTS.unpack_from(k, HEAD.size + 256)
    words = struct.unpack_from('<32I', k, HEAD.size + 256 + COUNTS.size)
    at = HEAD.size + 256 + COUNTS.size + 128
    entries = [entry(ENTRY.unpack_from(k, at + ENTRY.size * i), rec['samplers'], constexpr_tsc) for i in range(count)]
    root, status, arrays, _, slots, heap_bytes, *pairs = packfile.KERNEL_V20.unpack_from(k, at + ENTRIES * ENTRY.size)
    pairs, (spill_entry, spill_words, _, _) = pairs[:16], pairs[16:]
    stage_in_region, stage_in_strides = packfile.KERNEL_V21.unpack_from(
        k, at + ENTRIES * ENTRY.size + packfile.KERNEL_V20.size
    )
    return {
        'root_table_word': root,
        'status_word': status,
        'root_slots': slots,
        'heap': [{'class': pairs[2 * i], 'offset': pairs[2 * i + 1]} for i in range(arrays)],
        'heap_bytes': heap_bytes,
        'spill_entry': spill_entry,
        'spill': rec.get('spill', [])[:spill_words],
        'key': rec['key'],
        'region': region,
        'set_bytes': set_bytes,
        'read_only_entries': read_only,
        'qmd': qmd,
        'push': {w: words[w] for w in range(32) if mask >> w & 1},
        'entries': entries,
        'stage_in_region': stage_in_region,
        'stage_in_strides': stage_in_strides,
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


def state_facts(records: list[dict]) -> list[dict]:
    """Give each compute state's answers (directpack.mm's DKRN facts), once a state: every shape of it names the same."""
    facts: dict[int, dict] = {}
    for rec in records:
        if rec['outcome'] or 'facts' not in rec:
            continue
        state = packfile.fnv(rec['key'].split(' | tg=', 1)[0].encode())
        max_threads, width, static = rec['facts']
        made = {'hash': state, 'max_threads': max_threads, 'execution_width': width, 'static_memory': static}
        if facts.setdefault(state, made) != made:
            raise ValueError(f'compute state {state:x} answers otherwise at another shape: {rec["key"]}')
    return list(facts.values())


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
