"""The fetch-free forms' iterated blend programs in the pack (runtime/raw_pack.h PackIterated), for directlink.py.

A rung-3 form names its program (ca/fetchfree.py, iterated/table.py). The colour matrix (vcm) has no fixed program: the
GPU's macro loads it and its constants from the uniform each draw binds, so its form also names where that uniform is:
the fragment buffer whose bytes from a 16-byte boundary the storage form's program loads and the fetch-free form's does
not, five half4 rows then the clamp pair (QuartzCore 24B119's 44 bytes at 0x60 of buffer 1, where its texture function
leaves them there), or the five rows alone (21H461's 40 bytes at 0xb0: its matrix has no clamp, the macro's clamp pair
is then 0, its form without one). Loads are LDCs of the buffer's constant bank, read off the machine code.
"""

import struct

import table

MATRIX = 'vcm'
ROW_WORDS = 10  # five half4 rows
CLAMP_WORDS = 1  # the clamp pair (p3, p4) as two halves
# A rows-only matrix's name: its draws feed the macro a clamp pair of 0 (raw_gpu.cpp matrixConstants), a route of its
# own that raw_chip_facts.h kIteratedProven proves apart from vcm's
ROWS_ONLY = 'vcm.rows'
HEADER_BYTES = 0x50  # a graphics program's shader program header, ahead of its code
LDC_OPCODE = 0x1DF2  # bits 63-51 of an LDC (opcodes.txt: 1110 1111 1001 0)
# PackUniformRange: entry, bank, first, end
RANGE = struct.Struct('<BBBB')


def loads(image: bytes, bank: int) -> set[int]:
    """Give the byte offsets an LDC of constant bank `bank` reads (with its register, which the bind residual is)."""
    out = set()
    words = [struct.unpack_from('<Q', image, at)[0] for at in range(HEADER_BYTES, len(image) - 7, 8)]
    for i, w in enumerate(words):
        if i % 4 == 0 or w >> 51 != LDC_OPCODE or (w >> 36) & 0x1F != bank or (w >> 44) & 3:
            continue
        count = {4: 1, 5: 2, 6: 4}.get((w >> 48) & 7, 0)
        out |= {((w >> 20) & 0xFFFF) + 4 * k for k in range(count)}
    return out


def fragment_banks(pipe: dict) -> dict[int, int]:
    """Give the constant bank each fragment uniform buffer of a pipeline's fragment program is read through, by slot."""
    out = {}
    for i in range(len(pipe['uniform']) // RANGE.size):
        entry, bank, _first, _end = RANGE.unpack_from(pipe['uniform'], i * RANGE.size)
        if entry < len(pipe['entries']):
            e = pipe['entries'][entry]
            if e['kind'] == 'ubo' and e['stage'] == 'f' and not e['ssbo']:
                out[e['slot']] = bank
    return out


def matrix_uniform(storage: dict, free: dict) -> tuple[int, int, int] | None:
    """Give the colour matrix uniform's (fragment buffer index, byte offset, words), None where no buffer shows it.

    The rows: five half4 rows from a 16-byte boundary the storage form reads, none of which the fetch-free form reads,
    else (a texture function reading the same words, 21H461's c33, c4b, 3dl, mpc and opa) the rows holding every word
    the storage form reads there and the fetch-free form does not, else, where the storage form reads no word the
    fetch-free form does not (the blend's loads merged with its texture function's: 21H461's c4b), the one block of
    rows both read. Its words: the rows and the clamp pair where the storage form reads the pair too, else the rows
    alone.
    `storage`, `free`: each a pipeline's 'uniform' and 'entries' and its fragment program's 'fs_image'.
    """
    free_banks = fragment_banks(free)
    found, shared = [], []
    for slot, bank in fragment_banks(storage).items():
        read = loads(storage['fs_image'], bank)
        theirs = loads(free['fs_image'], free_banks[slot]) if slot in free_banks else set()
        for offset in sorted(read):
            rows = {offset + 4 * k for k in range(ROW_WORDS)}
            if offset % 16 or not rows <= read:
                continue
            clamp = offset + 4 * ROW_WORDS
            words = ROW_WORDS + (CLAMP_WORDS if clamp in read and clamp not in theirs else 0)
            if not rows & theirs:
                return slot, offset, words
            if read - theirs and read - theirs <= rows | {clamp}:
                found.append((slot, offset, words))
            elif read == theirs:
                shared.append((slot, offset, words))
    if found:
        return found[0]
    return shared[0] if len(shared) == 1 else None


def attach(pipes: list[dict], report: list[str]) -> tuple[list[dict], list[dict]]:
    """Give the pipelines and the PackIterated section's programs.

    Each fetch-free form's `iterated` becomes its program's index + 1; a colour matrix form whose uniform no buffer
    shows is left out.
    """
    storage = {p['key']: p for p in pipes if not p['form']}
    programs: dict[tuple, int] = {}
    records: list[dict] = []
    kept, unplaced = [], 0
    for p in pipes:
        name = p.pop('iterated_name', '')
        if not name:
            kept.append(p)
            continue
        if name == MATRIX:
            found = storage.get(p['key']) and matrix_uniform(storage[p['key']], p)
            if not found:
                unplaced += 1
                continue
            record = {
                'name': name if found[2] == ROW_WORDS + CLAMP_WORDS else ROWS_ONLY,
                'words': [],
                'enable': 3,
                'constants': {},
                'alpha_source': 0x4001,
                'alpha_dest': 0x4000,
                'uniform_slot': found[0],
                'uniform_offset': found[1],
                'uniform_words': found[2],
            }
        elif name in table.PROGRAMS:
            record = table.record(table.PROGRAMS[name])
        else:
            raise ValueError(f'a fetch-free form names an iterated blend program iterated/table.py lacks: {name}')
        identity = (name, record.get('uniform_slot'), record.get('uniform_offset'), record.get('uniform_words'))
        if identity not in programs:
            programs[identity] = len(records)
            records.append(record)
        p['iterated'] = programs[identity] + 1
        kept.append(p)
    for p in kept:
        del p['fs_image']
    free = [p for p in kept if p['form'] == 'free']
    rung3 = sum(1 for p in free if p.get('iterated'))
    report.append(
        f'{len(free)} fetch-free forms: {len(free) - rung3} on rung 2, {rung3} on rung 3 ({len(records)} iterated '
        f'blend programs); {unplaced} colour matrix forms left out: no uniform buffer shows the matrix past its reads'
    )
    return kept, records
