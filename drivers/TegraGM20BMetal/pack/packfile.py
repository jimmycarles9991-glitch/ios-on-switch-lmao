import functools
import json
import os
import re
import struct
from dataclasses import dataclass, field

import selector_section

MAGIC = b'GM20BPK1'
# 2: the header's stack key (make_pack.py --stack) after the section table; 3: barrier-form records; 4: programs' shader
# data and constant buffer types; 6: compute kernels (runtime/pack.h PackKernel); 8: a texture3d entry's Metal
# index; 9: split fragment programs (PackSplit); 10: dynamic blend push fields; 11: qualified ordered storage views;
# 15: program variants, programs' register counts and bound-texture slots, the uniform table, fetch rungs, chip facts,
# binary archives' declared states; 16: 32-bit program indices (pipelines, variants, kernels, splits); 17: fetch-free
# forms and their iterated blend programs; 18: a record's set-0 entries read from memory; 19: an iterated blend
# program's name and its matrix's words; 20: argument buffers (handle-table entries, root-table and status push words,
# root slots, set-1 heap arrays of states and kernels; argument tables of push fields past a stage's window, their
# words in the blob section); 21: the runtime records' tessellation, amplification, dynamic stride and stage-in
# fields (every pack state's are 0), a state's vertex streams past stream 0, layer and viewport count push words; 22:
# the formats a state's written textures were compiled for, the fragment slots it samples two-plane RGB10A8_2P_XR10
# textures at; 23: compute states' answers (PackComputeFacts), compared sampler slots; 24: a tessellation state's
# control and evaluation programs, its stride twin's key hash, an amplifying state's layer and viewport offset words,
# the registers a state's draws set back after it (pairs in the blob section); 25: states of no fragment program
# (NO_PROGRAM), the reflections section; 26: the texture slots a kernel's sampler entry samples (its window); 27: set-0
# entries past 32 (the count's high byte, the rest of the entries and their bound and memory bits in a far block in the
# blob section, an argument table's entry past 254), programs' uniform buffers past set-0 byte 496 (a far table in the
# blob section); 28: the selector section (PackSelectors), a state's shader-side stencil push word (shadow_stencil_word)
VERSION = 30
SECTIONS = 16  # runtime/pack.h PackSectionCount
ENTRY_COMPARES = 1  # runtime/pack.h kEntryCompares: a handle's sampler slot the state samples by comparison
# PackIteratedBlend: count, enable, constants, uniformSlot, uniformOffset, uniformWords, reserved, alphaSource,
# alphaDest, name, reservedName, constant[8][3], program[128]
ITERATED = struct.Struct('<BBBBHBBIIQQ24H128I')
KERNEL_ENTRIES = 64  # PackKernel.entry's length (pack.h kKernelEntries)
PROGRAM_CBUFS = 18  # PackProgram.cbufType's length (runtime/pack.h)
# PackPipeline's record-v15 tail: variants, variantCount[2], storageBlendMask, storageBlendModel, boundEntries,
# uniform[8], fetchRung[8], variantName
PIPELINE_V15 = struct.Struct('<IBBBBI32s8sI')
# PackPipeline's record-v18 tail: memoryEntries
PIPELINE_V18 = struct.Struct('<I')
ALL_ENTRIES = 0xFFFFFFFF  # a record whose programs' set reads no tool read: every entry read from memory
NO_PROGRAM = 0xFFFFFFFF  # runtime/pack.h kNoProgram: a state of no fragment program
# PackPipeline's record-v20 tail: rootTableWord[2], statusWord[2], rootSlots[2], heapArrays, heapBytes, heap[8] as
# (heapClass, offset) pairs, spillEntry[2], spillWords[2], spillAt
PIPELINE_V20 = struct.Struct('<2B2B2IB3xI16I2B2BI')
HEAP_ARRAYS = 8
# PackKernel's v20 tail: rootTableWord, statusWord, heapArrays, reserved, rootSlots, heapBytes, heap[8] pairs,
# spillEntry, spillWords, reserved, spillAt
KERNEL_V20 = struct.Struct('<4B2I16IBBHI')
# PackPipeline's record-v21 tail: its head (controlWord, controlPoints, controlPointIndexBytes, vertexStrideDynamic,
# amplificationMax, amplificationIdWord, amplificationCountWord), arrayLengthWord, pulledStride[8][2], vertexStreams[7],
# viewportCountWord
PIPELINE_V21 = struct.Struct('<7sB16s7sB')
VERTEX_STREAMS = 7  # PackPipeline.vertexStreams' length: the streams past stream 0
STREAM_BIND_STRIDE = 0x80  # runtime/pack.h kStreamBindStride
# PackKernel's record-v21 tail: stageInRegionWord, stageInStride[4][2], reserved
KERNEL_V21 = struct.Struct('<B8s3x')
# PackPipeline's record-v22 tail: writtenFormat[16], by fragment texture slot, xrSlots, reserved
WRITTEN_SLOTS = 16  # runtime/pack.h kWrittenSlots
PIPELINE_V22 = struct.Struct(f'<{WRITTEN_SLOTS}HH2x')
# PackPipeline's record-v24 tail: tessPrograms[2], strideTwin, amplificationLayerWord, amplificationViewportWord,
# restoreDwords, restoreAt
PIPELINE_V24 = struct.Struct('<2IQBBHI')
NO_UNIFORM = b'\xff\x00\x00\x00' * 8


def chip_facts() -> int:
    """Give runtime/chip_facts.h kProvenChipFacts: the chip facts device probes proved, stamped into the header."""
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'chip_facts.h')) as header:
        return int(re.search(r'kProvenChipFacts = (0x[0-9A-Fa-f]+|\d+)', header.read())[1], 0)


# PackSplit (runtime/pack.h)
PACK_SPLIT = struct.Struct('<IIIIHBBBB8sHH64IHH7I')
SPLIT_COST_FIELDS = (
    'version',
    'reserved',
    'launch_cycles',
    'drain_cycles',
    'prologue_warp_cycles',
    'prologue_instructions',
    'saved_pixel_instructions',
    'saved_pixel_cycles',
    'frontier_values',
)


def fnv(b: bytes, h: int = 0xCBF29CE484222325) -> int:
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# pack.h kPackEntries: the set-0 entries a PackPipeline holds; the rest are in its far block (set_entries.h)
PACK_ENTRIES = 32
SET_ENTRIES_MAX = 370  # set_entries.h kSetEntriesMax
FAR_AT_LIMIT = 1 << 24  # PackPipeline.farEntriesAt and PackProgram.farUbos: 24 bits
V20_FAR_AT = 13  # PackPipeline.farEntriesAt's byte in the PIPELINE_V20 tail (its heapReserved before v27)
V22_SPILL_HIGH = 2 * 16 + 2  # PackPipeline.spillEntryHigh's byte in the PIPELINE_V22 tail (its xrReserved before v27)
TABLE_KINDS = {'texture_table': 8, 'sampler_table': 9, 'spill_table': 10}  # pack.h PackEntryKind


def entry_bytes(entries: list[dict]) -> bytearray:
    """Give the entries' PackEntry bytes: the first PACK_ENTRIES padded to PACK_ENTRIES, then every one past them."""
    if len(entries) > SET_ENTRIES_MAX:
        raise ValueError(f'{len(entries)} set-0 entries: a record holds {SET_ENTRIES_MAX}')
    ents = bytearray()
    for e in entries:
        if e['kind'] == 'color_table':  # pack.h EntryColorTable
            ents += struct.pack('<HBBBBBB', e['off'], 5, 2, 0, 0, 0, 0)
        elif e['kind'] in TABLE_KINDS:  # EntryTextureTable, EntrySamplerTable, EntrySpillTable
            ents += struct.pack('<HBBBBBB', e['off'], TABLE_KINDS[e['kind']], e['stage'], 0, 0, 0, 0)
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
            tkind = 0 if tk is None else {'slot': 2, 'fetch': 3, 'tex3d': 4, 'depth_test': 6, 'stencil_test': 7}[tk[0]]
            skind = 0 if sk is None else (1 if sk[0] == 'slot' else 2)
            stage = 1 if e.get('stage') == 'v' else 2
            compares = ENTRY_COMPARES if e.get('compare') else 0  # pack.h kEntryCompares
            ents += struct.pack(
                '<HBBBBBB',
                e['off'],
                tkind,
                stage,
                0 if tk is None else tk[1],
                skind,
                0 if sk is None else sk[1],
                compares,
            )
    if len(entries) < PACK_ENTRIES:
        ents += bytes(8 * (PACK_ENTRIES - len(entries)))
    return ents


def far_words(rec: dict) -> list[int]:
    """Give a record's far block (set_entries.h): its entries past PACK_ENTRIES, then their bound and memory bits.

    A record of PACK_ENTRIES entries or fewer has none.
    """
    count = len(rec['entries'])
    if count <= PACK_ENTRIES:
        return []
    far = entry_bytes(rec['entries'])[8 * PACK_ENTRIES :]
    masks = (count - PACK_ENTRIES + 31) // 32
    bound = rec.get('bound_entries', 0) >> PACK_ENTRIES
    memory = rec.get('memory_entries', (1 << count) - 1) >> PACK_ENTRIES
    words = list(struct.unpack(f'<{len(far) // 4}I', far))
    return (
        words
        + [bound >> 32 * i & 0xFFFFFFFF for i in range(masks)]
        + [memory >> 32 * i & 0xFFFFFFFF for i in range(masks)]
    )


def far_at_bytes(at: int) -> bytes:
    if at >= FAR_AT_LIMIT:
        raise ValueError(f'a far block at blob word {at}: past the 24 bits that name it')
    return at.to_bytes(3, 'little')


def buffer_kind(e: dict) -> int:
    """Give a buffer entry's samplerKind byte (pack.h bufferStorage, bufferResidualWord)."""
    residual = e.get('residual')
    word = 0 if residual is None else ((residual - 0x700) // 4 + 1) << 2
    return word | (1 if e.get('ssbo') else 0)


def push_bytes(push: list[tuple[int, int]]) -> bytes:
    mask, words = 0, [0] * 32
    for o, val in push:
        mask |= 1 << ((o - 0x700) // 4)
        words[(o - 0x700) // 4] = val
    return struct.pack('<I32I', mask, *words)


# A key's variant name (runtime/pack.h variantNameAt): " | cubic ...", " | tests=<n>", " | xrs=<slots>",
# " | wtex=<slot>:<format>,..." at its end
VARIANT_NAME = re.compile(r'( \| cubic [^|]*?)?( \| tests=\d+)?( \| xrs=\d+)?( \| wtex=[\d:,]+)?$')


def variant_name(key: str) -> str:
    """Give the name of the variant a record's key names after its state's key, '' for the state's own records."""
    return VARIANT_NAME.search(key).group(0)


@functools.cache
def own_states() -> dict[str, int]:
    """Give each of the driver's own states (runtime/own_states.h kOwnStates) its pack key's hash.

    Keyed by the name the runtime asks for, raw.<family>.<fragment>: <family> is the vertex function's name minus its
    last word.
    """
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'own_states.h')) as header:
        text = header.read()
    table = text[text.index('kOwnStates[]') : text.index('};', text.index('kOwnStates[]'))]
    states = {}
    for pair in re.findall(r'"([^"]+)"', table):
        vertex, fragment = pair.split(' | f ')
        states[f'{vertex} | f {fragment}'] = fnv(f'raw.{vertex.rpartition("_")[0]}.{fragment}'.encode())
    return states


def key_hash(rec: dict) -> int:
    """Give the hash the runtime looks a record up by: its state's key's, a variant's too (PipelineVariantRecord)."""
    key = rec['key'][: len(rec['key']) - len(variant_name(rec['key']))]
    parts = key.split(' | ')
    pair = f'{parts[1].rstrip()[2:]} | {parts[2]}' if len(parts) == 3 and parts[1].startswith('v ') else ''
    return own_states().get(pair, fnv(key.encode()))


def pipeline_bytes(rec: dict, key_offset: int, blob_off: int, variants_off: int = 0, name_offset: int = 0) -> bytes:
    vb = rec['vbs'][0] if rec['vbs'] else {'slot': 0xFF, 'stride': 0}
    keyhash = key_hash(rec)
    flags = (1 if rec['fetch'] else 0) | (2 if rec.get('form') == 'barrier' else 0)  # PackPipelineFlags
    flags |= 4 if rec.get('push_metadata') else 0
    flags |= 8 if rec.get('storage_mask') else 0
    flags |= 16 if variant_name(rec['key']) else 0
    flags |= 32 if rec.get('form') == 'free' else 0
    flags |= 64 if rec.get('turn_refused') else 0  # PipelineTurnRefused
    flags |= 256 if rec.get('raster_ordered') else 0  # PipelineRasterOrdered
    head = struct.pack(
        '<QIIIIIBBBBHHI',
        keyhash,
        key_offset,
        rec['vs'],
        rec['fs'],
        blob_off,
        len(rec['blob']) * 2,
        len(rec['entries']) & 0xFF,
        len(rec['push']),
        vb['slot'],
        len(rec['entries']) >> 8,
        vb['stride'],
        rec['set_size'],
        flags,
    )
    dynamic = struct.pack('<HH', rec.get('half_blend_color_word', 0), rec.get('iterated', 0))
    storage = struct.pack(
        '<BBBB8I',
        rec.get('storage_mask', 0),
        rec.get('storage_write_mask', 0),
        rec.get('storage_samples', 0),
        rec.get('storage_tests', 0),
        *rec.get('storage_tic', [0] * 8),
    )
    tests = struct.pack(
        '<BBBBIII',
        rec.get('depth_test_word', 0),
        rec.get('stencil_test_word', 0),
        rec.get('test_flags', 0),
        rec.get('shadow_stencil_word', 0),
        rec.get('depth_tic', 0),
        rec.get('stencil_tic', 0),
        0,
    )
    counts = [sum(1 for v in rec.get('variants', []) if v['stage'] == stage) for stage in (0, 1)]
    v15 = PIPELINE_V15.pack(
        variants_off if any(counts) else 0,
        *counts,
        rec.get('storage_blend_mask', 0),
        rec.get('storage_blend_model', 0),
        rec.get('bound_entries', 0) & 0xFFFFFFFF,
        rec.get('uniform', NO_UNIFORM),
        bytes(rec.get('fetch_rung', [0] * 8)),
        name_offset,
    )
    v18 = PIPELINE_V18.pack(rec.get('memory_entries', ALL_ENTRIES) & 0xFFFFFFFF)
    entries = entry_bytes(rec['entries'])[: 8 * PACK_ENTRIES]
    tails = v15 + v18 + v20_tail(rec) + v21_tail(rec) + v22_tail(rec) + v24_tail(rec)
    return head + entries + push_bytes(rec['push']) + dynamic + storage + tests + tails


def v20_tail(rec: dict) -> bytes:
    """Give a record's PIPELINE_V20 tail: what its reads through argument buffers take of a draw."""
    heap = rec.get('heap', [])
    if len(heap) > HEAP_ARRAYS:
        raise ValueError(f'{len(heap)} set-1 heap arrays: a pipeline holds {HEAP_ARRAYS}')
    pairs = [word for array in heap for word in (array['class'], array['offset'])]
    tail = PIPELINE_V20.pack(
        *rec.get('root_table_word', [0, 0]),
        *rec.get('status_word', [0, 0]),
        *rec.get('root_slots', [0, 0]),
        len(heap),
        rec.get('heap_bytes', 0),
        *pairs,
        *[0] * (2 * HEAP_ARRAYS - len(pairs)),
        *[entry & 0xFF for entry in rec.get('spill_entry', [0, 0])],
        *[len(words) for words in rec.get('spill', [[], []])],
        rec.get('spill_at', 0) if any(rec.get('spill', [[], []])) else 0,
    )
    if len(rec['entries']) <= PACK_ENTRIES:
        return tail
    far_at = far_at_bytes(rec['far_at'])
    return tail[:V20_FAR_AT] + far_at + tail[V20_FAR_AT + len(far_at) :]


def v21_tail(rec: dict) -> bytes:
    """Give a record's PIPELINE_V21 tail: its tessellation, stride and amplification fields, streams and pass words."""
    return PIPELINE_V21.pack(
        bytes(rec.get('runtime_head', bytes(7))),
        rec.get('array_length_word', 0),
        bytes(rec.get('pulled_stride', bytes(16))),
        bytes(rec.get('vertex_streams', [])).ljust(VERTEX_STREAMS, b'\0'),
        rec.get('viewport_count_word', 0),
    )


def v22_tail(rec: dict) -> bytes:
    """Give a record's PIPELINE_V22 tail: written textures' formats, xrSlots and argument tables' entry high bytes."""
    tail = PIPELINE_V22.pack(*rec.get('written_format', [0] * WRITTEN_SLOTS), rec.get('xr_slots', 0))
    vertex, fragment = rec.get('spill_entry', [0, 0])
    high = struct.pack('<H', vertex >> 8 | (fragment >> 8) << 8)
    return tail[:V22_SPILL_HIGH] + high + tail[V22_SPILL_HIGH + 2 :]


def v24_tail(rec: dict) -> bytes:
    """Give a record's PIPELINE_V24 tail: its tessellation programs, stride twin, amplification words, restore pairs."""
    restore = rec.get('restore', [])
    return PIPELINE_V24.pack(
        *rec.get('tess', [0, 0]),
        rec.get('stride_twin', 0),
        rec.get('amplification_layer_word', 0),
        rec.get('amplification_viewport_word', 0),
        2 * len(restore),
        rec.get('restore_at', 0) if restore else 0,
    )


def kernel_window(e: dict) -> int:
    """Give a kernel entry's window word: a buffer's window bytes, its residual word + 1 in the top byte (0: none)."""
    residual = e.get('residual')
    return e['window'] | (residual + 1) << 24 if residual is not None else e['window']


def kernel_bytes(k: dict, key_offset: int, spill_at: int = 0) -> bytes:
    """Give a compute kernel's PackKernel: its key, program, QMD template, set size, push words and set-0 entries."""
    mask, words = 0, [0] * 32
    for w, v in k['push'].items():
        mask |= 1 << w
        words[w] = v
    entries = b''.join(struct.pack('<HBBI', e['off'], e['kind'], e['slot'], kernel_window(e)) for e in k['entries'])
    entries += bytes(8 * (KERNEL_ENTRIES - len(k['entries'])))
    heap = k.get('heap', [])
    pairs = [word for array in heap for word in (array['class'], array['offset'])]
    arguments = KERNEL_V20.pack(
        k.get('root_table_word', 0),
        k.get('status_word', 0),
        len(heap),
        k.get('maps_stated', 0),
        k.get('root_slots', 0),
        k.get('heap_bytes', 0),
        *pairs,
        *[0] * (2 * HEAP_ARRAYS - len(pairs)),
        k.get('spill_entry', 0),
        len(k.get('spill', [])),
        0,
        spill_at if k.get('spill') else 0,
    )
    return (
        struct.pack('<QIIBb', fnv(k['key'].encode()), key_offset, k['program'], len(k['entries']), k['region'])
        + struct.pack('<64I', *k['qmd'])
        + struct.pack('<HQI', k['set_bytes'], k.get('read_only_entries', 0), mask)
        + struct.pack('<32I', *words)
        + entries
        + arguments
        + KERNEL_V21.pack(k.get('stage_in_region', 0), k.get('stage_in_strides', bytes(8)))
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
            'gprs': pr.get('gprs', 0),
            'bound_textures': pr.get('bound_textures', 0),
            'far_ubos': pr.get('far_ubos', []),
        }
    )


def iterated_bytes(programs: list[dict]) -> bytes:
    """Give the PackIterated section: each fetch-free form's iterated blend (iterated/table.py record)."""
    out = bytearray()
    for p in programs:
        constants = [half for j in range(8) for half in p['constants'].get(j, (0, 0, 0))]
        words = p['words'] + [0] * (128 - len(p['words']))
        out += ITERATED.pack(
            len(p['words']),
            p['enable'],
            len(p['constants']),
            p.get('uniform_slot', 0xFF),
            p.get('uniform_offset', 0),
            p.get('uniform_words', 0),
            0,
            p['alpha_source'],
            p['alpha_dest'],
            fnv(p['name'].encode()),
            0,
            *constants,
            *words,
        )
    return bytes(out)


def split_bytes(found: list[dict]) -> bytes:
    """Give the PackSplit section: the splits in whole-program order (pack.cpp Pack::split searches it)."""
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
            s.get('batch_version', 0),
            s.get('batch_threads', 0),
            *s['qmd'],
            *(s.get('cost', {}).get(field, 0) for field in SPLIT_COST_FIELDS),
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
    archives: bytes = b''  # the PackArchives section (archives.py)
    iterated: list[dict] = field(default_factory=list)  # the PackIterated section's programs (iterated_bytes)
    compute_facts: list[dict] = field(default_factory=list)  # the PackComputeStates section (kernels.py state_facts)
    # the PackReflections section's blobs (reflection_bytes): by (compute, the state's key hash)
    reflections: dict[tuple[bool, int], bytes] = field(default_factory=dict)


def pipeline_sections(pipes: list[dict]) -> tuple[bytearray, list[int], bytearray, bytearray]:
    """Give the pipelines, the blob, the strings and the variants.

    The blob holds the registers' pairs, one copy of each distinct run, each record's argument tables' words, the
    vertex table's first (its spill_at), and the pairs its draws set back after it (its restore_at).
    """
    strings = bytearray()

    def string(s: str) -> int:
        off = len(strings)
        strings.extend(s.encode() + b'\0')
        return off

    blob, blobs = [], {}
    pipe_bin, variant_bin = bytearray(), bytearray()
    for rec in pipes:
        pairs = tuple(rec['blob'])
        if pairs not in blobs:
            blobs[pairs] = len(blob)
            for m, v in pairs:
                blob += [m, v]
        variants_off = len(variant_bin) // 8
        for v in sorted(rec.get('variants', []), key=lambda v: v['stage']):
            variant_bin += struct.pack('<II', v['assume'], v['program'])
        # the key's attachment and vertex fields (what the runtime reads of it: gpu.cpp Device::pipeline), not the
        # functions' constants (some 1.5 KB a fixed-function state; pack.json has the whole key)
        name = variant_name(rec['key'])
        key_off = string(rec['key'].split(' | ')[0].rstrip())
        spill_at = len(blob)
        for words in rec.get('spill', [[], []]):
            blob += words
        restore_at = len(blob)
        for m, v in rec.get('restore', []):
            blob += [m, v]
        far_at = len(blob)
        blob += far_words(rec)
        placed = dict(rec, spill_at=spill_at, restore_at=restore_at, far_at=far_at)
        pipe_bin += pipeline_bytes(placed, key_off, blobs[pairs], variants_off, string(name) if name else 0)
    return pipe_bin, blob, strings, variant_bin


def compute_facts_bytes(facts: list[dict]) -> bytes:
    """Give the PackComputeStates section: each compute state's answers, ascending by its key's hash."""
    return b''.join(
        struct.pack('<QIIII', f['hash'], f['max_threads'], f['execution_width'], f['static_memory'], 0)
        for f in sorted(facts, key=lambda f: f['hash'])
    )


def reflection_bytes(reflections: dict[tuple[bool, int], bytes]) -> bytes:
    """Give the PackReflections section (runtime/pack.h PackReflection).

    Its counts, the render states' entries, then the compute states', each ascending by key hash, then each distinct
    blob once, 8-byte aligned.
    """
    render = sorted((h, b) for (compute, h), b in reflections.items() if not compute)
    compute = sorted((h, b) for (computed, h), b in reflections.items() if computed)
    head = struct.pack('<II', len(render), len(compute))
    at = len(head) + 16 * (len(render) + len(compute))
    entries, blobs, placed = bytearray(), bytearray(), {}
    for h, b in render + compute:
        if b not in placed:
            blobs += bytes(-(at + len(blobs)) % 8)
            placed[b] = at + len(blobs)
            blobs += b
        entries += struct.pack('<QII', h, placed[b], len(b))
    return head + bytes(entries) + bytes(blobs)


def pack_bytes(pack: Pack) -> tuple[bytes, list[int]]:
    pipe_bin, blob, strings, variant_bin = pipeline_sections(pack.pipes)
    kernel_bin = bytearray()
    for k in sorted(pack.kernels, key=lambda k: fnv(k['key'].encode())):
        kernel_bin += kernel_bytes(k, len(strings), len(blob))
        blob += k.get('spill', [])
        strings.extend(k['key'].encode() + b'\0')
    prog_bin = bytearray()
    for pr in pack.programs:
        types = bytes(pr['cbufs']) + bytes(PROGRAM_CBUFS - len(pr['cbufs']))
        far = b'\0\0\0'
        if any(pr.get('far_ubos', [])):
            far = far_at_bytes(len(blob) + 1)
            table = list(pr['far_ubos']) + [0] * (PROGRAM_CBUFS - len(pr['far_ubos']))
            blob += list(struct.unpack(f'<{PROGRAM_CBUFS // 2}I', struct.pack(f'<{PROGRAM_CBUFS}H', *table)))
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
            + struct.pack('<BB', pr.get('gprs', 0), pr.get('bound_textures', 0))
            + far
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
        bytes(variant_bin),
        pack.archives,
        iterated_bytes(pack.iterated),
        compute_facts_bytes(pack.compute_facts),
        reflection_bytes(pack.reflections),
        selector_section.section_bytes(pack.pipes),
    ]
    assert len(sections) == SECTIONS
    header_size = 8 + 4 * 4 + 8 * 2 * len(sections) + 16  # the section table, then the stack key and chip facts
    offs, at = [], (header_size + 255) // 256 * 256
    body = bytearray()
    for s in sections:
        pad = (-(at + len(body))) % 256
        body += bytes(pad)
        offs.append((at + len(body), len(s)))
        body += s
    hdr = MAGIC + struct.pack('<IIII', VERSION, len(pack.programs), len(pack.pipes), len(pack.constexpr_tsc))
    hdr += struct.pack(f'<{2 * len(sections)}Q', *[x for o in offs for x in o])
    hdr += struct.pack('<QII', 0, chip_facts(), 0)  # the stack key (make_pack.py stamps it), chip facts, reserved
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
    return 'u' if e['kind'] in ('ubo', 'color_table', 'spill_table') else ('h' if e.get('smp') else 't')


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
                if fs is not None and fs != NO_PROGRAM:
                    f.write(f'{vs} {name(pack.programs[fs])} {rec["set_size"]:x} {slots}\n')
