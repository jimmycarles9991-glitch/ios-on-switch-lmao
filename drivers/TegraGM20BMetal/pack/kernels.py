"""The compute kernels directpack.mm made directly, for directlink.py.

A DKRN record holds its PackKernel with program index 0, its program (code, shader data, constant buffer types) and
sampler headers, which its sampler entries name as CONSTEXPR | n, each with the texture slots it samples in its window.
The pack numbers those headers among its constexpr samplers, and a buffer entry's window keeps its residual word + 1 in
its top byte (runtime/pack.h).
"""

import re
import struct

import packfile

CONSTEXPR = 0x80  # PackKernelEntry.slot of a constexpr sampler: its index in the pack's constexpr samplers
# runtime/pack.h PackKernelEntryKind
BUFFER, TEXTURE, SAMPLER, TEXTURE_TABLE, SAMPLER_TABLE, SPILL_TABLE, TEXTURE_PLANE = range(7)
TWO_PLANES = 'xr2p'  # a tex= field's mark of an RGB10A8_2P_XR10 texture held in two planes (runtime/compute_key.h)
PLANE_BYTES = {TEXTURE: 4, TEXTURE_PLANE: 1}  # a two-plane texture's color words (R32Uint) and alpha bytes (R8Uint)
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


# Header word 0's COMPONENTS (clb097tex.h): a texel's bytes (runtime/view.h texelBytes)
TEXEL_BYTES = {
    0x01: 16,
    0x02: 12,
    0x03: 8,
    0x04: 8,
    0x05: 8,
    0x07: 4,
    0x08: 4,
    0x09: 4,
    0x0C: 4,
    0x0D: 4,
    0x0E: 4,
    0x0F: 4,
    0x20: 4,
    0x21: 4,
    0x12: 2,
    0x13: 2,
    0x14: 2,
    0x15: 2,
    0x16: 2,
    0x18: 2,
    0x1B: 2,
    0x1D: 1,
    0x1E: 1,
}
# MTLPixelFormat: a texel's bytes, the uncompressed colour formats a kernel binds
FORMAT_BYTES = {
    **dict.fromkeys((10, 11, 12, 13, 14), 1),
    **dict.fromkeys((20, 22, 23, 24, 25, 30, 31, 32, 33, 34, 40, 41, 42, 43), 2),
    **dict.fromkeys((53, 54, 55, 60, 62, 63, 64, 65, 70, 71, 72, 73, 74, 80, 81, 90, 91, 92, 93, 94, 554, 555), 4),
    **dict.fromkeys((103, 104, 105, 110, 112, 113, 114, 115, 552, 553), 8),
    **dict.fromkeys((123, 124, 125), 16),
}


def views_other_texel_size(key: str, entries: list[dict]) -> list[int]:
    """Give the texture slots a record views in texels of another size than the format its key binds there.

    A two-plane texture's slot views its planes: its color words at the slot's entry, its alpha bytes at its plane
    entry. A plane entry at a slot of another texture is named too. The runtime binds no such view (runtime/view.h
    SlotView::None).
    """
    found = re.search(r'\btex=([^ |]*)', key)
    fields = [t.split(':') for t in found.group(1).split(',') if t] if found else []
    bound = {int(f[0]): int(f[1]) for f in fields}
    planes = {int(f[0]) for f in fields if f[-1] == TWO_PLANES}
    other = []
    for e in entries:
        if e['kind'] not in PLANE_BYTES or not e['window']:
            continue
        texel = TEXEL_BYTES.get(e['window'] & 0x7F)
        if e['slot'] in planes:
            fmt = PLANE_BYTES[e['kind']]
        elif e['kind'] == TEXTURE_PLANE:
            other.append(e['slot'])
            continue
        else:
            fmt = FORMAT_BYTES.get(bound.get(e['slot'], -1))
        if texel and fmt and texel != fmt:
            other.append(e['slot'])
    return other


def entry(fields: tuple[int, int, int, int], headers: list[list[int]], constexpr_tsc: list[list[int]]) -> dict:
    """Give a record's set-0 entry as packfile.py takes it, its sampler numbered among the pack's."""
    off, kind, slot, window = fields
    if kind == SAMPLER:
        header = headers[slot & (CONSTEXPR - 1)]
        if header not in constexpr_tsc:
            constexpr_tsc.append(header)
        return {'off': off, 'kind': kind, 'slot': CONSTEXPR | constexpr_tsc.index(header), 'window': window}
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
    root, status, arrays, maps_stated, slots, heap_bytes, *pairs = packfile.KERNEL_V20.unpack_from(
        k, at + ENTRIES * ENTRY.size
    )
    pairs, (spill_entry, spill_words, _, _) = pairs[:16], pairs[16:]
    stage_in_region, stage_in_strides = packfile.KERNEL_V21.unpack_from(
        k, at + ENTRIES * ENTRY.size + packfile.KERNEL_V20.size
    )
    return {
        'root_table_word': root,
        'status_word': status,
        'maps_stated': maps_stated,
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
        made = kernel(rec, constexpr_tsc)
        if slots := views_other_texel_size(rec['key'], made['entries']):
            skipped.append((rec['key'], f'texture slots {slots} viewed in texels of another size'))
            continue
        kernels.append(made)
    return kernels, skipped


def state_facts(records: list[dict]) -> list[dict]:
    """Give each compute state's DKRN answers once: every dispatch shape must carry the same answers."""
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
