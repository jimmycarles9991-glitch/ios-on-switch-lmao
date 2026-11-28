import array
import functools
import hashlib
import json
import os
import re
import struct
import sys
from dataclasses import dataclass, field

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
# 31: the selector section gone; 32: the fast keys section (PackFastKeys); 33: two tiers (the header's tier0 table, each
# section's tier 0 first, a stride twin as its record's index + 1), programs' facts (PackProgram.facts); 34: pre-built
# tier-0 state segments in tier 0 of PackHeap (PackSegments) and tier0SlmPerWarp in PackHeader; 35: programs' header
# facts (PackProgram.facts), a variant's set-0 memory reads (PackVariant.memoryEntries), the segments' words block
# (PackSegmentTable); 36: a record's buffers' Metal address words (PIPELINE_V36); 37: the rules' identity, 16 bytes
# after the tier-0 table (rules_identity), and a segment's blend kill
# (segments.FLAG_BLEND_KILL)
VERSION = 37
SECTIONS = 17  # runtime/pack.h PackSectionCount
ENTRY_COMPARES = 1  # runtime/pack.h kEntryCompares: a handle's sampler slot the state samples by comparison
ENTRY_ARRAY_VIEW = 2  # runtime/pack.h kEntryArrayView: a handle bound as a 2D-array view (m2v cube_as_array)
# PackIteratedBlend: count, enable, constants, uniformSlot, uniformOffset, uniformWords, reserved, alphaSource,
# alphaDest, name, reservedName, constant[8][3], program[128]
ITERATED = struct.Struct('<BBBBHBBIIQQ24H128I')
KERNEL_ENTRIES = 64  # PackKernel.entry's length (pack.h kKernelEntries)
PROGRAM_CBUFS = 18  # PackProgram.cbufType's length (runtime/pack.h)
PROGRAM_BYTES = 48  # sizeof(PackProgram)
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
# PackPipeline's record-v36 tail: guestBaseWord[4], guestBaseSlot[4] (m2v's SlotGuestBase fields)
GUEST_BASES = 4  # runtime/pack.h kPackGuestBases
GUEST_BASE_FRAGMENT = 0x80  # runtime/pack.h kGuestBaseFragment
PIPELINE_V36 = struct.Struct(f'<{GUEST_BASES}s{GUEST_BASES}s')
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
TEX_KINDS = {'slot': 2, 'fetch': 3, 'tex3d': 4, 'depth_test': 6, 'stencil_test': 7}
ENTRY_HANDLE = struct.Struct('<HBBBBBB')
ENTRY_UBO = struct.Struct('<HBBBBH')
PUSH_STRUCT = struct.Struct('<I32I')
PIPE_HEAD = struct.Struct('<QIIIIIBBBBHHI')
PIPE_DYNAMIC = struct.Struct('<HH')
PIPE_STORAGE = struct.Struct('<BBBB8I')
PIPE_TESTS = struct.Struct('<BBBBIII')
VARIANT_STRUCT = struct.Struct('<III')
FAST_KEY_STRUCT = struct.Struct('<QII')
SPILL_HIGH_STRUCT = struct.Struct('<H')
PROG_RECORD = struct.Struct('<IIIIIB18sBB3sI')
FAR_UBOS_PACK = struct.Struct(f'<{PROGRAM_CBUFS}H')
FAR_UBOS_UNPACK = struct.Struct(f'<{PROGRAM_CBUFS // 2}I')
TSC_STRUCT = struct.Struct('<8I')
BASE_STRUCT = struct.Struct('<II')
INIT_STRUCT = struct.Struct('<HHII')
_ZERO_8 = [0] * 8
_ZERO_16 = [0] * 16
_ZERO_7B = bytes(7)
_ZERO_16B = bytes(16)
_ZERO_8B = bytes(8)
_CBUF_PAD = tuple(bytes(PROGRAM_CBUFS - i) for i in range(PROGRAM_CBUFS + 1))


def entry_bytes(entries: list[dict]) -> bytearray:
    """Give the entries' PackEntry bytes: the first PACK_ENTRIES padded to PACK_ENTRIES, then every one past them."""
    count = len(entries)
    if count > SET_ENTRIES_MAX:
        raise ValueError(f'{count} set-0 entries: a record holds {SET_ENTRIES_MAX}')
    ents = bytearray(8 * (count if count > PACK_ENTRIES else PACK_ENTRIES))
    pack_handle = ENTRY_HANDLE.pack_into
    pack_ubo = ENTRY_UBO.pack_into
    for i, e in enumerate(entries):
        off = i * 8
        kind = e['kind']
        if kind == 'color_table':  # pack.h EntryColorTable
            pack_handle(ents, off, e['off'], 5, 2, 0, 0, 0, 0)
        elif kind in TABLE_KINDS:  # EntryTextureTable, EntrySamplerTable, EntrySpillTable
            pack_handle(ents, off, e['off'], TABLE_KINDS[kind], e['stage'], 0, 0, 0, 0)
        elif kind == 'ubo':
            pack_ubo(
                ents,
                off,
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
            tkind = 0 if tk is None else TEX_KINDS[tk[0]]
            skind = 0 if sk is None else (1 if sk[0] == 'slot' else 2)
            stage = 1 if e.get('stage') == 'v' else 2
            pad = (ENTRY_COMPARES if e.get('compare') else 0) | (ENTRY_ARRAY_VIEW if e.get('array_view') else 0)
            pack_handle(
                ents,
                off,
                e['off'],
                tkind,
                stage,
                0 if tk is None else tk[1],
                skind,
                0 if sk is None else sk[1],
                pad,
            )
    return ents


def far_words(rec: dict, encoded_entries: bytearray | None = None) -> list[int]:
    """Give a record's far block (set_entries.h): its entries past PACK_ENTRIES, then their bound and memory bits.

    A record of PACK_ENTRIES entries or fewer has none.
    """
    count = len(rec['entries'])
    if count <= PACK_ENTRIES:
        return []
    far = (entry_bytes(rec['entries']) if encoded_entries is None else encoded_entries)[8 * PACK_ENTRIES :]
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
        idx = (o - 0x700) >> 2
        mask |= 1 << idx
        words[idx] = val
    return PUSH_STRUCT.pack(mask, *words)


# A key's variant name (runtime/pack.h variantNameAt): " | cubic ...", " | tests=<n>", " | xrs=<slots>",
# " | wtex=<slot>:<format>,..." at its end
_WTEX_CHARS = frozenset('0123456789:,')


def variant_name(key: str) -> str:
    """Give the name of the variant a record's key names after its state's key, '' for the state's own records."""
    end = len(key)
    pos = key.rfind(' | ', 0, end)
    if pos >= 0:
        seg = key[pos + 3 : end]
        if seg.startswith('wtex=') and len(seg) > 5 and all(c in _WTEX_CHARS for c in seg[5:]):
            end = pos
            pos = key.rfind(' | ', 0, end)
    if pos >= 0:
        seg = key[pos + 3 : end]
        if seg.startswith('xrs=') and len(seg) > 4 and seg[4:].isdecimal():
            end = pos
            pos = key.rfind(' | ', 0, end)
    if pos >= 0:
        seg = key[pos + 3 : end]
        if seg.startswith('tests=') and len(seg) > 6 and seg[6:].isdecimal():
            end = pos
            pos = key.rfind(' | ', 0, end)
    if pos >= 0:
        seg = key[pos + 3 : end]
        if seg.startswith('cubic ') and '|' not in seg:
            end = pos
    return key[end:]


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


def key_hash(rec: dict, vname: str | None = None) -> int:
    """Give the hash the runtime looks a record up by: its state's key's, a variant's too (PipelineVariantRecord)."""
    raw = rec['key']
    if vname is None:
        vname = variant_name(raw)
    key = raw[: len(raw) - len(vname)] if vname else raw
    bar1 = key.find(' | ')
    if bar1 >= 0 and key.startswith('v ', bar1 + 3):
        bar2 = key.find(' | ', bar1 + 3)
        if bar2 >= 0 and key.find(' | ', bar2 + 3) < 0:
            pair = f'{key[bar1 + 5 : bar2].rstrip()} | {key[bar2 + 3 :]}'
            hit = own_states().get(pair)
            if hit is not None:
                return hit
    return fnv(key.encode())


def pipeline_bytes(
    rec: dict,
    key_offset: int,
    blob_off: int,
    variants_off: int = 0,
    name_offset: int = 0,
    *,
    keyhash: int | None = None,
    vname: str | None = None,
    entries_head: bytes | bytearray | None = None,
    spill_at: int | None = None,
    restore_at: int | None = None,
    far_at: int | None = None,
    stride_twin: int | None = None,
) -> bytes:
    vbs = rec['vbs']
    vb_slot, vb_stride = (vbs[0]['slot'], vbs[0]['stride']) if vbs else (0xFF, 0)
    if vname is None:
        vname = variant_name(rec['key'])
    if keyhash is None:
        keyhash = key_hash(rec, vname)
    form = rec.get('form')
    storage_mask = rec.get('storage_mask', 0)
    flags = (1 if rec['fetch'] else 0) | (2 if form == 'barrier' else 0)  # PackPipelineFlags
    if rec.get('push_metadata'):
        flags |= 4
    if storage_mask:
        flags |= 8
    if vname:
        flags |= 16
    if form == 'free':
        flags |= 32
    if rec.get('turn_refused'):
        flags |= 64  # PipelineTurnRefused
    if rec.get('raster_ordered'):
        flags |= 256  # PipelineRasterOrdered
    entry_count = len(rec['entries'])
    head = PIPE_HEAD.pack(
        keyhash,
        key_offset,
        rec['vs'],
        rec['fs'],
        blob_off,
        len(rec['blob']) * 2,
        entry_count & 0xFF,
        len(rec['push']),
        vb_slot,
        entry_count >> 8,
        vb_stride,
        rec['set_size'],
        flags,
    )
    dynamic = PIPE_DYNAMIC.pack(rec.get('half_blend_color_word', 0), rec.get('iterated', 0))
    storage = PIPE_STORAGE.pack(
        storage_mask,
        rec.get('storage_write_mask', 0),
        rec.get('storage_samples', 0),
        rec.get('storage_tests', 0),
        *rec.get('storage_tic', _ZERO_8),
    )
    tests = PIPE_TESTS.pack(
        rec.get('depth_test_word', 0),
        rec.get('stencil_test_word', 0),
        rec.get('test_flags', 0),
        rec.get('shadow_stencil_word', 0),
        rec.get('depth_tic', 0),
        rec.get('stencil_tic', 0),
        0,
    )
    variants = rec.get('variants')
    if variants:
        c0 = sum(1 for v in variants if v['stage'] == 0)
        c1 = sum(1 for v in variants if v['stage'] == 1)
    else:
        c0 = c1 = 0
    fetch_rung = rec.get('fetch_rung')
    v15 = PIPELINE_V15.pack(
        variants_off if (c0 or c1) else 0,
        c0,
        c1,
        rec.get('storage_blend_mask', 0),
        rec.get('storage_blend_model', 0),
        rec.get('bound_entries', 0) & 0xFFFFFFFF,
        rec.get('uniform', NO_UNIFORM),
        bytes(fetch_rung) if fetch_rung is not None else _ZERO_8B,
        name_offset,
    )
    v18 = PIPELINE_V18.pack(rec.get('memory_entries', ALL_ENTRIES) & 0xFFFFFFFF)
    entries = entry_bytes(rec['entries'])[: 8 * PACK_ENTRIES] if entries_head is None else entries_head
    tails = (
        v15
        + v18
        + v20_tail(rec, spill_at=spill_at, far_at=far_at)
        + v21_tail(rec)
        + v22_tail(rec)
        + v24_tail(rec, restore_at=restore_at, stride_twin=stride_twin)
        + v36_tail(rec)
    )
    return head + entries + push_bytes(rec['push']) + dynamic + storage + tests + tails


def v20_tail(rec: dict, *, spill_at: int | None = None, far_at: int | None = None) -> bytes:
    """Give a record's PIPELINE_V20 tail: what its reads through argument buffers take of a draw."""
    heap = rec.get('heap', ())
    if len(heap) > HEAP_ARRAYS:
        raise ValueError(f'{len(heap)} set-1 heap arrays: a pipeline holds {HEAP_ARRAYS}')
    pairs = [word for array in heap for word in (array['class'], array['offset'])] if heap else ()
    spill = rec.get('spill', ((), ()))
    spill_entry = rec.get('spill_entry', (0, 0))
    spill_at_val = (rec.get('spill_at', 0) if spill_at is None else spill_at) if (spill[0] or spill[1]) else 0
    tail = PIPELINE_V20.pack(
        *rec.get('root_table_word', (0, 0)),
        *rec.get('status_word', (0, 0)),
        *rec.get('root_slots', (0, 0)),
        len(heap),
        rec.get('heap_bytes', 0),
        *pairs,
        *([0] * (2 * HEAP_ARRAYS - len(pairs)) if len(pairs) < 2 * HEAP_ARRAYS else ()),
        spill_entry[0] & 0xFF,
        spill_entry[1] & 0xFF,
        len(spill[0]),
        len(spill[1]),
        spill_at_val,
    )
    if len(rec['entries']) <= PACK_ENTRIES:
        return tail
    far_at_bytes_val = far_at_bytes(rec['far_at'] if far_at is None else far_at)
    return tail[:V20_FAR_AT] + far_at_bytes_val + tail[V20_FAR_AT + len(far_at_bytes_val) :]


def v21_tail(rec: dict) -> bytes:
    """Give a record's PIPELINE_V21 tail: its tessellation, stride and amplification fields, streams and pass words."""
    runtime_head = rec.get('runtime_head')
    pulled_stride = rec.get('pulled_stride')
    vertex_streams = rec.get('vertex_streams')
    return PIPELINE_V21.pack(
        bytes(runtime_head) if runtime_head is not None else _ZERO_7B,
        rec.get('array_length_word', 0),
        bytes(pulled_stride) if pulled_stride is not None else _ZERO_16B,
        bytes(vertex_streams).ljust(VERTEX_STREAMS, b'\0') if vertex_streams else _ZERO_7B,
        rec.get('viewport_count_word', 0),
    )


def v22_tail(rec: dict) -> bytes:
    """Give a record's PIPELINE_V22 tail: written textures' formats, xrSlots and argument tables' entry high bytes."""
    tail = PIPELINE_V22.pack(*rec.get('written_format', _ZERO_16), rec.get('xr_slots', 0))
    vertex, fragment = rec.get('spill_entry', (0, 0))
    if vertex <= 0xFF and fragment <= 0xFF:
        return tail
    high = SPILL_HIGH_STRUCT.pack(vertex >> 8 | (fragment >> 8) << 8)
    return tail[:V22_SPILL_HIGH] + high + tail[V22_SPILL_HIGH + 2 :]


def v24_tail(rec: dict, *, restore_at: int | None = None, stride_twin: int | None = None) -> bytes:
    """Give a record's PIPELINE_V24 tail: its tessellation programs, stride twin, amplification words, restore pairs."""
    restore = rec.get('restore')
    return PIPELINE_V24.pack(
        *rec.get('tess', (0, 0)),
        rec.get('stride_twin', 0) if stride_twin is None else stride_twin,
        rec.get('amplification_layer_word', 0),
        rec.get('amplification_viewport_word', 0),
        2 * len(restore) if restore else 0,
        (rec.get('restore_at', 0) if restore_at is None else restore_at) if restore else 0,
    )


def v36_tail(rec: dict) -> bytes:
    """Give a record's PIPELINE_V36 tail: the push words each draw writes a buffer slot's Metal address to, and the slots."""
    return PIPELINE_V36.pack(bytes(rec.get('guest_base_word', ())), bytes(rec.get('guest_base_slot', ())))


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


_PROGRAM_HASHES: dict[bytes, int] = {}


def program_hash(program: dict) -> int:
    """Give the FNV-1a of a program's header, code and shader data, memoised by a C digest."""
    image = program['image']
    data = program['data']
    h = hashlib.blake2b(digest_size=16)
    h.update(image)
    if data:
        h.update(data)
    digest = h.digest()
    known = _PROGRAM_HASHES.get(digest)
    if known is None:
        v = fnv(image)
        if data:
            v = fnv(data, v)
        known = _PROGRAM_HASHES[digest] = v
    return known


def program_name(program: dict) -> str:
    """Give the name nvdec gives a bound program: the FNV-1a of its header, code and shader data.

    The pack's ids hold its constant buffer types too (directlink.py program_id), which nvdec cannot see.
    """
    return f'{program_hash(program):x}'


def place_graphics(
    pid: str, programs: dict[str, dict], heap: bytearray, prog_index: dict[str, int], prog_recs: list[dict]
) -> None:
    """Append a 3D program to the heap image as NVK uploads it pre-Turing: header at +0x30, code at +0x80."""
    if pid in prog_index:
        return
    pr = programs[pid]
    storage_id = pr.get('storage_id', pid)
    # The first record of each storage id, under a key no program id takes (prog_index is only looked up by key):
    # one probe where a scan of every record placed made the link quadratic in programs
    shared = prog_index.get(('storage', storage_id))
    if shared is not None:
        prog_index[pid] = len(prog_recs)
        prog_recs.append(dict(prog_recs[shared], id=pid))
        return
    prog_index['storage', storage_id] = len(prog_recs)
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
            'facts': pr.get('facts', 0),
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
    # the PackFastKeys section's (fast_key_bytes): each state's fast key, the key hash of its records
    fast_keys: dict[int, int] = field(default_factory=dict)
    # tier 0's programs and heap bytes (directlink.py lay_out_tiers); None: every program is tier 0's
    tier0_programs: int | None = None
    tier0_heap: int | None = None
    segments: bytes = b''  # the PackSegments section (segments.py)
    tier0_slm_per_warp: int = 0


# The heap section starts on this boundary (runtime/pack.h kPackTierAlign), so its tier 0 ends on one in the file too
TIER_ALIGN = 128 << 10
# The sections whose tier 0 is the whole section: read at a process's open, or as a whole (the blob's runs are shared)
WHOLE_SECTIONS = (2, 3, 4, 5, 12, 16)  # samplers, baseline, init, blob, iterated, segments
# The sections no state of the launch set reads at its creation: their tier 0 is empty
UNTIERED_SECTIONS = (8, 11, 13, 14)  # kernels, archives, compute states, reflections
SECTION_NAMES = (
    'programs',
    'pipelines',
    'samplers',
    'baseline',
    'init',
    'blob',
    'strings',
    'heap',
    'kernels',
    'splits',
    'variants',
    'archives',
    'iterated',
    'compute states',
    'reflections',
    'fast keys',
    'segments',
)


def twin_indices(pipes: list[dict], *, hashes: list[int] | None = None, vnames: list[str] | None = None) -> list[int]:
    """Give each record's stride twin as the pack names it: the index + 1 of its twin's record of its form, 0: none.

    As Pack::find picks a state's record of a form: no variant record; the free form's or the barrier form's own; else
    the storage form's record, else the first.
    """
    runs: dict[int, list[int]] = {}
    for at, rec in enumerate(pipes):
        vname = variant_name(rec['key']) if vnames is None else vnames[at]
        if not vname:
            h = key_hash(rec, vname) if hashes is None else hashes[at]
            runs.setdefault(h, []).append(at)

    def find(key: int, form: str) -> int:
        ats = runs.get(key, ())
        if form in ('free', 'barrier'):
            return next((at for at in ats if pipes[at].get('form') == form), -1)
        plain = [at for at in ats if pipes[at].get('form') not in ('free', 'barrier')]
        return next((at for at in plain if pipes[at].get('storage_mask')), plain[0] if plain else -1)

    return [find(rec['stride_twin'], rec.get('form', '')) + 1 if rec.get('stride_twin') else 0 for rec in pipes]


def pipeline_sections(
    pipes: list[dict],
    tier0: list[int] | None = None,
    *,
    hashes: list[int] | None = None,
    vnames: list[str] | None = None,
) -> tuple[bytearray, list[int], bytearray, bytearray]:
    """Give the pipelines, the blob, the strings and the variants; `tier0` gets tier 0's strings and variant bytes.

    The blob holds the registers' pairs, one copy of each distinct run, each record's argument tables' words, the
    vertex table's first (its spill_at), and the pairs its draws set back after it (its restore_at).
    """
    strings = bytearray()
    strings_extend = strings.extend
    strings_append = strings.append

    def string(s: str) -> int:
        off = len(strings)
        strings_extend(s.encode())
        strings_append(0)
        return off

    if vnames is None:
        vnames = [variant_name(rec['key']) for rec in pipes]
    if hashes is None:
        hashes = [key_hash(rec, vn) for rec, vn in zip(pipes, vnames, strict=True)]
    blob: list[int] = []
    blobs: dict[tuple[tuple[int, int], ...], int] = {}
    pipe_bin, variant_bin = bytearray(), bytearray()
    pack_variant = VARIANT_STRUCT.pack
    twins = twin_indices(pipes, hashes=hashes, vnames=vnames)
    for rec, twin, keyhash, name in zip(pipes, twins, hashes, vnames, strict=True):
        if tier0 is not None and not tier0 and rec.get('tier', 0):
            tier0 += [len(strings), len(variant_bin)]
        pairs = tuple(rec['blob'])
        blob_off = blobs.get(pairs)
        if blob_off is None:
            blob_off = blobs[pairs] = len(blob)
            for m, v in pairs:
                blob.append(m)
                blob.append(v)
        variants_off = len(variant_bin) // VARIANT_STRUCT.size
        variants = rec.get('variants')
        if variants:
            if len(variants) > 1:
                variants = sorted(variants, key=lambda v: v['stage'])
            for v in variants:
                variant_bin += pack_variant(v['assume'], v['program'], v.get('memory', ALL_ENTRIES))
        # the key's attachment and vertex fields (what the runtime reads of it: gpu.cpp Device::pipeline), not the
        # functions' constants (some 1.5 KB a fixed-function state; pack.json has the whole key)
        key = rec['key']
        bar = key.find(' | ')
        key_off = string((key[:bar] if bar >= 0 else key).rstrip())
        spill_at = len(blob)
        spill = rec.get('spill')
        if spill:
            for words in spill:
                if words:
                    blob.extend(words)
        restore_at = len(blob)
        restore = rec.get('restore')
        if restore:
            for m, v in restore:
                blob.append(m)
                blob.append(v)
        far_at = len(blob)
        ents = entry_bytes(rec['entries'])
        if len(rec['entries']) > PACK_ENTRIES:
            blob.extend(far_words(rec, ents))
        pipe_bin += pipeline_bytes(
            rec,
            key_off,
            blob_off,
            variants_off,
            string(name) if name else 0,
            keyhash=keyhash,
            vname=name,
            entries_head=ents[: 8 * PACK_ENTRIES],
            spill_at=spill_at,
            restore_at=restore_at,
            far_at=far_at,
            stride_twin=twin,
        )
    if tier0 is not None and not tier0:
        tier0 += [len(strings), len(variant_bin)]
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


def fast_key_bytes(
    fast_keys: dict[int, int], pipes: list[dict], *, hashes: list[int] | None = None
) -> tuple[bytes, int]:
    """Give the PackFastKeys section (runtime/pack.h PackFastKey) and its tier-0 rows: by tier, then fast key.

    Each state's records are one run of the pipelines, which are in key hash order within each tier; a fast key whose
    state has no record is left out.
    """
    runs: dict[int, tuple[int, int, int]] = {}
    last = (-1, -1)
    for at, rec in enumerate(pipes):
        h = key_hash(rec) if hashes is None else hashes[at]
        tier_val = rec.get('tier', 0)
        order = (tier_val, h)
        if order < last:
            raise ValueError(f'pipeline {at} ({h:016x}) after {last[1]:016x}: not in tier and key hash order')
        prev = runs.get(h)
        runs[h] = (tier_val, at, 1) if prev is None else (prev[0], prev[1], prev[2] + 1)
        last = order
    rows = sorted((runs[h][0], fast, runs[h][1], runs[h][2]) for fast, h in fast_keys.items() if h in runs)
    pack_row = FAST_KEY_STRUCT.pack
    return b''.join(pack_row(row[1], row[2], row[3]) for row in rows), sum(1 for row in rows if row[0] == 0)


def tier0_prefixes(pack: Pack, strings: int, variants: int, fast_rows: int, sections: list[bytes]) -> list[int]:
    """Give each section's tier-0 bytes (runtime/pack.h PackHeader::tier0)."""
    records = sum(1 for rec in pack.pipes if rec.get('tier', 0) == 0)
    programs = len(pack.programs) if pack.tier0_programs is None else pack.tier0_programs
    heap = len(pack.heap) if pack.tier0_heap is None else pack.tier0_heap
    tier0 = [len(s) if i in WHOLE_SECTIONS else 0 for i, s in enumerate(sections)]
    tier0[0] = programs * PROGRAM_BYTES
    tier0[1] = records * len(sections[1]) // max(len(pack.pipes), 1)
    tier0[6], tier0[7], tier0[10] = strings, heap, variants
    tier0[9] = sum(1 for split in pack.splits if split['whole'] < programs) * PACK_SPLIT.size
    tier0[15] = fast_rows * 16
    return tier0


def sections_line(data: bytes) -> str:
    """Give the report's line of each section's bytes and its tier 0's, from a pack's header."""
    count = len(SECTION_NAMES)
    offsets = struct.unpack_from(f'<{2 * count}Q', data, 24)
    tier0 = struct.unpack_from(f'<{count}Q', data, 24 + 16 * count + 16)
    return 'pack sections (KiB, tier 0 KiB): ' + ', '.join(
        f'{name} {offsets[2 * i + 1] // 1024} ({tier0[i] // 1024})' for i, name in enumerate(SECTION_NAMES)
    )


def rules_identity(pack: Pack) -> bytes:
    """The first 16 bytes of the SHA-256 of the rules a device-made record is made by (record_parts.cpp
    PackRules::of): the registers the pack's states set apart, the baseline, and the context its init leaves, as
    little-endian words, each list's count first. The shader compiler names its store by it (compiler_store.h)."""
    varying = sorted({m for rec in pack.pipes for m, _ in rec['blob']})
    context: dict[int, int] = {}
    for subc, method, data, reloc in pack.init:
        if subc == 0 and reloc == 0 and data:
            context[method] = data
        elif subc == 0:
            context.pop(method, None)
    words = [len(varying), *varying]
    for registers in (pack.baseline, context):
        words += [len(registers), *[x for m, v in sorted(registers.items()) for x in (m, v)]]
    return hashlib.sha256(struct.pack(f'<{len(words)}I', *words)).digest()[:16]


def pack_bytes(pack: Pack, hashes: list[int] | None = None) -> tuple[bytes, list[int]]:
    tier0_strings_variants: list[int] = []
    vnames = [variant_name(rec['key']) for rec in pack.pipes]
    if hashes is None:
        hashes = [key_hash(rec, vn) for rec, vn in zip(pack.pipes, vnames, strict=True)]
    pipe_bin, blob, strings, variant_bin = pipeline_sections(
        pack.pipes, tier0_strings_variants, hashes=hashes, vnames=vnames
    )
    kernel_bin = bytearray()
    for k in sorted(pack.kernels, key=lambda k: fnv(k['key'].encode())):
        kernel_bin += kernel_bytes(k, len(strings), len(blob))
        blob += k.get('spill', [])
        strings.extend(k['key'].encode())
        strings.append(0)
    prog_bin = bytearray(PROGRAM_BYTES * len(pack.programs))
    pack_prog = PROG_RECORD.pack_into
    for i, pr in enumerate(pack.programs):
        cbufs = pr['cbufs']
        types = bytes(cbufs) + _CBUF_PAD[len(cbufs)]
        far = b'\0\0\0'
        far_ubos = pr.get('far_ubos')
        if far_ubos and any(far_ubos):
            far = far_at_bytes(len(blob) + 1)
            table = list(far_ubos) + [0] * (PROGRAM_CBUFS - len(far_ubos))
            blob.extend(FAR_UBOS_UNPACK.unpack(FAR_UBOS_PACK.pack(*table)))
        pack_prog(
            prog_bin,
            i * PROGRAM_BYTES,
            pr['heap_offset'],
            pr['program'],
            pr['bytes'],
            pr['data_offset'],
            pr['data_bytes'],
            len(cbufs),
            types,
            pr.get('gprs', 0),
            pr.get('bound_textures', 0),
            far,
            pr.get('facts', 0),
        )
    tsc_bin = b''.join(TSC_STRUCT.pack(*w) for w in pack.constexpr_tsc)
    base_bin = b''.join(BASE_STRUCT.pack(m, v) for m, v in sorted(pack.baseline.items()))
    init_bin = b''.join(INIT_STRUCT.pack(sc, m, d, rc) for sc, m, d, rc in pack.init)
    if sys.byteorder == 'little' and array.array('I').itemsize == 4:
        blob_bin = array.array('I', blob).tobytes()
    else:
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
    ]
    fast_bin, fast_rows = fast_key_bytes(pack.fast_keys, pack.pipes, hashes=hashes)
    sections.append(fast_bin)
    sections.append(pack.segments)
    assert len(sections) == SECTIONS
    tier0 = tier0_prefixes(pack, *tier0_strings_variants, fast_rows, sections)
    # the section table, then the stack key, chip facts and tier0SlmPerWarp, then the tier-0 table and the rules' identity
    header_size = 8 + 4 * 4 + 8 * 2 * len(sections) + 16 + 8 * len(sections) + 16
    offs, at = [], (header_size + 255) // 256 * 256
    body = bytearray()
    for i, s in enumerate(sections):
        pad = (-(at + len(body))) % (TIER_ALIGN if i == 7 else 256)
        body += bytes(pad)
        offs.append((at + len(body), len(s)))
        body += s
    hdr = MAGIC + struct.pack('<IIII', VERSION, len(pack.programs), len(pack.pipes), len(pack.constexpr_tsc))
    hdr += struct.pack(f'<{2 * len(sections)}Q', *[x for o in offs for x in o])
    hdr += struct.pack('<QII', 0, chip_facts(), pack.tier0_slm_per_warp)  # stack (make_pack.py), chip facts, tier0 SLM
    hdr += struct.pack(f'<{len(sections)}Q', *tier0)
    hdr += rules_identity(pack)
    return hdr + bytes((header_size + 255) // 256 * 256 - len(hdr)) + body, blob


def _json_default(o: object) -> str:
    return o.hex() if isinstance(o, (bytes, bytearray)) else str(o)


def _write_json_list(out, items: list, encode, chunk: int = 512) -> None:
    if not items:
        out.write('[]')
        return
    out.write('[\n')
    for i in range(0, len(items), chunk):
        encoded = encode(items[i : i + chunk])
        # JSON escapes string newlines as \n, so every raw newline here is structural indentation to shift by one space.
        inner = encoded[2:-2].replace('\n', '\n ')
        if i:
            out.write(',\n')
        out.write(' ')
        out.write(inner)
    out.write('\n ]')


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
    encode = json.JSONEncoder(indent=1, default=_json_default).encode
    with open(path, 'w') as pack_json:
        pack_json.write('{\n')
        items = list(js.items())
        for idx, (key, val) in enumerate(items):
            if idx:
                pack_json.write(',\n')
            pack_json.write(f' {encode(key)}: ')
            if isinstance(val, list):
                _write_json_list(pack_json, val, encode)
            else:
                pack_json.write(encode(val).replace('\n', '\n '))
        pack_json.write('\n}')


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
