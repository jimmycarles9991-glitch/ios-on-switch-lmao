#!/usr/bin/env python3
"""directlink: the pack (runtime/pack.h) from the direct compile's records (directpack.mm), by mkpack's rules.

A record holds the 3D registers emit makes for its state. Every other register is the context a draw meets: the
channel's registers after its context init (context.log), with what draw_context.txt changes of them, over which each
state's own are laid. mkpack.py's split_registers then tells the per-pipeline registers from the baseline, and the
programs, constexpr samplers and records are laid out as mkpack.py lays out the probe's.

usage: directlink.py <out dir> [--archives <inventory.tsv>] <bucket dir>...
(make_pack.py's buckets: records.bin holds states and kernels; bucket 0 holds context.log, the context init as nvdec ran
it: context_state.cpp; the inventory gives the binary archives' declared states, archives.py)
writes <out dir>/pack.bin, pack.json, layouts.txt, programs.bin (every render program, for nvdec) and report.txt
"""

import functools
import hashlib
import os
import struct
import sys
import time
from collections.abc import Callable

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'iterated'))
import archives
import fetchfree_link
import kernels
import mkpack
import packfile
import segments
import splits

PIPELINE = struct.Struct('<QIIIIIBBBBHHI')
PROGRAM_IDS: dict[bytes, str] = {}  # program_id's: a 16-byte BLAKE2b of the named bytes -> their FNV-1a in hex
# SET_PIPELINE_PROGRAM(vertex B, pixel): where each program sits in the shader heap, which the probe saw differ by state
PROGRAM_OFFSETS = (0x2044, 0x2144)
PIPELINE_BYTES = (
    PIPELINE.size
    + packfile.PACK_ENTRIES * 8
    + 4
    + 32 * 4
    + 4
    + 36
    + 16
    + packfile.PIPELINE_V15.size
    + packfile.PIPELINE_V18.size
    + packfile.PIPELINE_V20.size
    + packfile.PIPELINE_V21.size
    + packfile.PIPELINE_V22.size
    + packfile.PIPELINE_V24.size
    + packfile.PIPELINE_V36.size
)
# Where each record tail starts in a PackPipeline
V36_AT = PIPELINE_BYTES - packfile.PIPELINE_V36.size
V24_AT = V36_AT - packfile.PIPELINE_V24.size
V22_AT = V24_AT - packfile.PIPELINE_V22.size
V21_AT = V22_AT - packfile.PIPELINE_V21.size
V20_AT = V21_AT - packfile.PIPELINE_V20.size
ENTRY_UBO, ENTRY_COLOR_TABLE = 1, 5
TABLE_KINDS = {code: kind for kind, code in packfile.TABLE_KINDS.items()}
# SET_VERTEX_STREAM_A_FORMAT(0), 16 bytes apart, and its STRIDE field (bits 11:0)
STREAM_FORMAT, STREAM_STRIDE = 0x1C00, 0xFFF
TEXTURE_KINDS = {2: 'slot', 3: 'fetch', 4: 'tex3d', 6: 'depth_test', 7: 'stencil_test'}
SAMPLER_SLOT, SAMPLER_CONSTEXPR = 1, 2
# The heap's tier 0 ends on this boundary (runtime/pack.h kPackTierAlign) with an instruction prefetch's room after its
# last slot (runtime/program_pages.h kProgramPrefetchBytes)
TIER_ALIGN, PREFETCH = 128 << 10, 0x1000


U32_STRUCT = struct.Struct('<I')
DFKY_STRUCT = struct.Struct('<QQQ')
FACTS_STRUCT = struct.Struct('<3I')
HEADER_STRUCT = struct.Struct('<8I')
ENTRY_UNPACK = struct.Struct('<HBBBBBB').unpack_from
PUSH_UNPACK = struct.Struct('<33I').unpack_from
HALF_UNPACK = struct.Struct('<HH').unpack_from
STORAGE_UNPACK = struct.Struct('<BBBB8I').unpack_from
TESTS_UNPACK = struct.Struct('<BBBBIII').unpack_from
SPILL_HIGH_UNPACK = struct.Struct('<H').unpack_from


class Reader:
    def __init__(self, data: bytes):
        self.data, self.at = data, 0

    def take(self, n: int) -> bytes:
        at = self.at
        self.at = at + n
        return self.data[at : at + n]

    def u32(self) -> int:
        val = U32_STRUCT.unpack_from(self.data, self.at)[0]
        self.at += 4
        return val

    def blob(self) -> bytes:
        n = U32_STRUCT.unpack_from(self.data, self.at)[0]
        start = self.at + 4
        self.at = start + n
        return self.data[start : start + n]

    def words(self) -> list[int]:
        n = U32_STRUCT.unpack_from(self.data, self.at)[0]
        start = self.at + 4
        self.at = start + n
        return list(struct.unpack_from(f'<{n // 4}I', self.data, start)) if n else []

    def headers(self) -> list[list[int]]:
        n = U32_STRUCT.unpack_from(self.data, self.at)[0]
        start = self.at + 4
        self.at = start + n
        unpack = HEADER_STRUCT.unpack_from
        return [list(unpack(self.data, start + 32 * i)) for i in range(n // 32)]

    def done(self) -> bool:
        return self.at >= len(self.data)


def program(r: Reader) -> dict:
    image = r.blob()
    data = r.blob()
    cbufs = list(r.blob())
    far_n = r.u32()
    far_start = r.at
    r.at = far_start + far_n
    far_ubos = list(struct.unpack_from(f'<{far_n // 2}H', r.data, far_start)) if far_n else []
    words = r.u32()
    return {
        'image': image,
        'data': data,
        'cbufs': cbufs,
        'far_ubos': far_ubos,
        'gprs': words & 0xFF,
        'bound_textures': words >> 8 & 0xFF,
        'facts': words >> 16,
    }


def words_of(blob: bytes) -> list[int]:
    return list(struct.unpack(f'<{len(blob) // 4}I', blob)) if blob else []


def headers(blob: bytes) -> list[list[int]]:
    unpack = HEADER_STRUCT.unpack_from
    return [list(unpack(blob, 32 * i)) for i in range(len(blob) // 32)]


def load_records(path: str) -> tuple[list[dict], list[dict], dict[tuple[bool, int], bytes]]:
    """Give a records.bin's states, kernels and reflections (directpack.mm's layout).

    A reflection is a state's: a render state's under its key's hash, a compute state's under its computeStateKey's
    (a kernel key's part before its dispatch shape, as kernels.state_facts names it).
    """
    return read_records(path)[:3]


@functools.cache  # each bucket's records.bin is parsed once: gather, the kernels and main's reflections share it
def read_records(
    path: str,
) -> tuple[list[dict], list[dict], dict[tuple[bool, int], bytes], list[tuple[int, int, int]]]:
    """Give load_records' and each corpus line's fast key, its state's key hash and its line's tier (DFKY)."""
    with open(path, 'rb') as f:
        r = Reader(f.read())
    states, kernel_recs, reflections, fast = [], [], {}, []
    while not r.done():
        tag = r.take(4)
        if tag == b'DFKY':
            fast.append(DFKY_STRUCT.unpack(r.blob()))
            continue
        if tag == b'DRFL':
            compute, key, blob = bool(r.u32()), r.blob().decode(), r.blob()
            state = key.split(' | tg=', 1)[0] if compute else key
            reflections.setdefault((compute, packfile.fnv(state.encode())), blob)
            continue
        if tag not in (b'DREC', b'DKRN'):
            raise ValueError(f'{path}: no record at byte {r.at - 4}')
        outcome = r.u32()
        rec = {'outcome': outcome, 'key': r.blob().decode(), 'why': r.blob().decode()}
        if tag == b'DKRN':
            if outcome == 0:
                rec['kernel'] = r.blob()
                rec['program'] = {'image': bytes(80) + r.blob(), 'data': r.blob(), 'cbufs': list(r.blob())}
                rec['samplers'] = r.headers()
                rec['spill'] = r.words()
                rec['facts'] = FACTS_STRUCT.unpack(r.blob())
            kernel_recs.append(rec)
            continue
        if outcome == 0:
            rec['pipeline'] = r.blob()
            rec['key_string'] = r.blob().decode()
            reg_n = r.u32()
            reg_start = r.at
            r.at = reg_start + reg_n
            words = struct.unpack_from(f'<{reg_n // 4}I', r.data, reg_start) if reg_n else ()
            rec['regs'] = dict(zip(words[0::2], words[1::2], strict=True))
            rec['programs'] = [program(r), program(r)]
            rec['samplers'] = r.headers()
            if r.u32():
                rec['split'] = {'record': r.blob(), 'programs': [program(r), program(r)]}
            rec['variants'] = [{'assume': r.u32(), 'program': program(r), 'memory': r.u32()} for _ in range(r.u32())]
            rec['iterated'] = r.blob().decode()
            rec['spill'] = r.words()
            mask, push = r.u32(), r.words()
            rec['bound'] = {'mask': mask, 'push': push, 'spill': [r.words(), r.words()]}
            rec['tess_programs'] = [program(r) for _ in range(r.u32())]
            extras_w, restore_w = r.words(), r.words()
            rec['extras'] = list(zip(extras_w[0::2], extras_w[1::2], strict=True))
            rec['restore'] = list(zip(restore_w[0::2], restore_w[1::2], strict=True))
            rec['far'] = r.words()
        states.append(rec)
    return states, kernel_recs, reflections, fast


def fast_keys(lines: list[tuple[int, int, int]]) -> tuple[dict[int, int], dict[int, int]]:
    """Give each fast key its state's key hash, and each state the lowest tier of its lines.

    The lines are directpack.mm's DFKY (fast key, key hash, tier), one per corpus line. A fast key of two states, or a
    state of two fast keys, is refused: the fast key has to name what the key's text names
    (TegraGM20BMetalPipelineKey.h fastKey), no more and no less.
    """
    by_fast: dict[int, int] = {}
    by_state: dict[int, int] = {}
    tiers: dict[int, int] = {}
    for fast, state, tier in lines:
        if by_fast.setdefault(fast, state) != state:
            raise ValueError(f'fast key {fast:016x} names states {by_fast[fast]:016x} and {state:016x}')
        if by_state.setdefault(state, fast) != fast:
            raise ValueError(f'state {state:016x} has fast keys {by_state[state]:016x} and {fast:016x}')
        tiers[state] = min(tier, tiers.get(state, tier))
    return by_fast, tiers


def set_tiers(states: list[dict], tiers: dict[int, int], key_hashes: dict[int, int] | None = None) -> None:
    """Give each made state its tier: its lines' (`tiers`), 0 for a state no line names (the driver's own).

    A stride twin is in the lowest tier of its own and those of the states it is the twin of, so a state's twin is never
    of a tier after it.
    """
    hashes = [packfile.key_hash(rec) for rec in states]
    if key_hashes is not None:
        for rec, h in zip(states, hashes, strict=True):
            key_hashes[id(rec)] = h
    for rec, h in zip(states, hashes, strict=True):
        rec['tier'] = tiers.get(h, 0)
    twins: dict[int, int] = {}
    unpack_v24 = packfile.PIPELINE_V24.unpack_from
    for rec in states:
        twin = unpack_v24(rec['pipeline'], V24_AT)[2]
        if twin:
            twins[twin] = min(rec['tier'], twins.get(twin, rec['tier']))
    for rec, h in zip(states, hashes, strict=True):
        rec['tier'] = min(rec['tier'], twins.get(h, rec['tier']))


def lay_out_tiers(
    states: list[dict],
    programs: dict[str, dict],
    found: dict[str, dict],
    report: list[str],
    pid_cache: dict[int, str] | None = None,
) -> tuple[tuple, list[dict], int, int]:
    """Give the heap image `laid`, tier 0's programs first, the splits, and tier 0's programs and bytes.

    Each tier's generic programs, then its program variants, then its splits. Tier 0's part of the heap ends on a
    TIER_ALIGN boundary with PREFETCH bytes or more after its last slot (empty: no tier 0).
    """
    laid: tuple = (bytearray(), {}, [])
    split: list[dict] = []
    split_done: set[str] = set()
    tier0 = (0, 0)
    for tier in (0, 1):
        group = [rec for rec in states if rec['tier'] == tier]
        report.append(f'tier {tier}: {len(group)} states')
        mkpack.place_programs([rec['probe'] for rec in group], programs, laid)
        lay_out_variants(group, programs, laid, report, pid_cache)
        used = [_pid(rec['programs'][1], pid_cache) for rec in group if rec['programs'][1]['image']]
        fresh = [pid for pid in dict.fromkeys(used) if pid not in split_done]
        split_done.update(fresh)
        split += splits.lay_out(fresh, found, programs, laid, report)
        heap = laid[0]
        if tier == 0 and heap:
            heap += bytes(-(len(heap) + PREFETCH) % TIER_ALIGN + PREFETCH)
            tier0 = (len(laid[2]), len(heap))
    heap, _, prog_recs = laid
    data = sum(1 for pr in prog_recs if pr['data_bytes'])
    report.append(f'{len(prog_recs)} programs ({data} with shader data), shader heap image {len(heap) // 1024} KiB')
    return laid, sorted(split, key=lambda s: s['whole']), *tier0


def stage_programs(rec: dict) -> list[dict]:
    """Give a record's vertex program and its fragment program, which a state of no fragment function has none of."""
    return [p for p in rec['programs'] if p['image']]


def program_id(program: dict) -> str:
    """Name a program by its header, code, shader data and constant buffer types (FNV-1a).

    One code can read its uniform buffers at other set-0 offsets in another state (its constant buffer types): each is
    a program of its own, so neither state binds the other's.
    """
    image = program['image']
    data = program['data']
    cbufs = bytes(program['cbufs'])
    identity = program.get('identity', b'')
    far_ubos = program.get('far_ubos')
    far = struct.pack(f'<{len(far_ubos)}H', *far_ubos) if far_ubos else b''
    # A program comes here once per state, variant and split that uses it (over a million times a pack): its FNV-1a, a
    # Python loop over every byte, is made once and found again by a C digest of the same bytes
    h = hashlib.blake2b(digest_size=16)
    h.update(image)
    if data:
        h.update(data)
    if cbufs:
        h.update(cbufs)
    if identity:
        h.update(identity)
    if far:
        h.update(far)
    digest = h.digest()
    known = PROGRAM_IDS.get(digest)
    if known is None:
        v = packfile.program_hash(program)
        if cbufs:
            v = packfile.fnv(cbufs, v)
        if identity:
            v = packfile.fnv(identity, v)
        if far:
            v = packfile.fnv(far, v)
        known = PROGRAM_IDS[digest] = f'{v:x}'
    return known


def _pid(program: dict, pid_cache: dict[int, str] | None) -> str:
    if pid_cache is None:
        return program_id(program)
    key = id(program)
    known = pid_cache.get(key)
    if known is None:
        known = pid_cache[key] = program_id(program)
    return known


def entries_of(
    pipeline: bytes,
    count: int,
    samplers: list[list[int]],
    constexpr_tsc: list[list[int]],
    far: bytes = b'',
    tsc_index: dict[tuple[int, ...], int] | None = None,
) -> list[dict]:
    """Give a record's set-0 entries as mkpack.py's pipeline() makes them, constexpr samplers numbered in the pack's.

    Its entries past packfile.PACK_ENTRIES are in `far`, its far block's (set_entries.h).
    """
    out = []
    unpack_entry = ENTRY_UNPACK
    pipe_base = PIPELINE.size
    pack_entries = packfile.PACK_ENTRIES
    for i in range(count):
        if i < pack_entries:
            off, kind, stage, slot, sampler_kind, sampler, pad = unpack_entry(pipeline, pipe_base + 8 * i)
        else:
            off, kind, stage, slot, sampler_kind, sampler, pad = unpack_entry(far, 8 * (i - pack_entries))
        if kind == ENTRY_COLOR_TABLE:
            out.append({'off': off, 'kind': 'color_table'})
        elif kind in TABLE_KINDS:
            out.append({'off': off, 'kind': TABLE_KINDS[kind], 'stage': stage})
        elif kind == ENTRY_UBO:
            residual = sampler_kind >> 2
            out.append(
                {
                    'off': off,
                    'kind': 'ubo',
                    'stage': 'v' if stage == 1 else 'f',
                    'slot': slot,
                    'ssbo': bool(sampler_kind & 1),
                    'residual': 0x700 + 4 * (residual - 1) if residual else None,
                    'window': (sampler | pad << 8) * 16,
                }
            )
        else:
            entry = {'off': off, 'kind': 'handle', 'tex': (TEXTURE_KINDS[kind], slot) if kind else None, 'smp': None}
            if stage == 1:
                entry['stage'] = 'v'
            if pad & packfile.ENTRY_ARRAY_VIEW:
                entry['array_view'] = True
            if sampler_kind == SAMPLER_SLOT:
                entry['smp'] = ('slot', sampler)
                entry['compare'] = bool(pad & packfile.ENTRY_COMPARES)
            elif sampler_kind == SAMPLER_CONSTEXPR:
                header = samplers[sampler]
                if tsc_index is not None:
                    hkey = tuple(header)
                    idx = tsc_index.get(hkey)
                    if idx is None:
                        idx = tsc_index[hkey] = len(constexpr_tsc)
                        constexpr_tsc.append(header)
                else:
                    if header not in constexpr_tsc:
                        constexpr_tsc.append(header)
                    idx = constexpr_tsc.index(header)
                entry['smp'] = ('constexpr', idx)
            out.append(entry)
    return out


def layout_strides(key: str) -> dict[int, int]:
    """Give each Metal vertex buffer's layout stride, as the key's a<i>=format:offset:buffer:stride:... fields say."""
    out = {}
    bar = key.find(' | ')
    head = key[:bar] if bar >= 0 else key
    for field in head.split():
        if field[:1] == 'a':
            eq = field.find('=')
            if eq > 1 and field[1:eq].isdigit():
                parts = field[eq + 1 :].split(':')
                if len(parts) == 6 and all(p.isdigit() for p in parts):
                    out[int(parts[2])] = int(parts[3])
    return out


def check_streams(key: str, slot: int, dynamic: bool, streams: list[int], regs: dict[int, int]) -> None:
    """Refuse a record whose fixed-function vertex stream fetches at another stride than its layout's.

    m2v pulls a layout past maxVertexInputBindingStride (2048) in the shader, so no stream's stride is past the format's
    12 bits; a record that says otherwise would draw at its stride's low bits. A stream of a dynamic stride (stream 0's
    `dynamic`, a STREAM_BIND_STRIDE stream) holds none: each draw writes its bind's.
    """
    buffers = ([(slot, dynamic)] if slot != 0xFF else []) + [
        ((s & ~packfile.STREAM_BIND_STRIDE) - 1, bool(s & packfile.STREAM_BIND_STRIDE)) for s in streams
    ]
    if not buffers:
        return
    strides = layout_strides(key)
    for k, (buffer, bind_stride) in enumerate(buffers):
        stride = 0 if bind_stride else strides.get(buffer)
        held = regs.get(STREAM_FORMAT + 0x10 * k, 0) & STREAM_STRIDE
        if stride is None or stride > STREAM_STRIDE or held != stride:
            raise ValueError(f'vertex stream {k} of buffer {buffer} at stride {held}, its layout {stride}: {key}')


def pipeline_of(
    rec: dict,
    varying: list[int],
    prog_index: dict[str, int],
    constexpr_tsc: list[list[int]],
    *,
    pid_cache: dict[int, str] | None = None,
    tsc_index: dict[tuple[int, ...], int] | None = None,
    varying_by_targets: dict[int, list[int]] | None = None,
) -> dict:
    """Give a state's pipeline as mkpack.py's pipeline() gives the probe's."""
    p = rec['pipeline']
    if len(p) != PIPELINE_BYTES:
        raise ValueError('pipeline record has an obsolete layout')
    _hash, _key, _vs, _fs, _blob, _dwords, entries, _pushes, slot, entries_high, stride, set_bytes, flags = (
        PIPELINE.unpack_from(p)
    )
    entries |= entries_high << 8
    far = rec.get('far', ())
    far_entries = max(entries - packfile.PACK_ENTRIES, 0)
    masks = (far_entries + 31) // 32
    if len(far) != 2 * far_entries + 2 * masks:
        raise ValueError(f'{rec["key"]}: a far block of {len(far)} words for {entries} entries')
    far_bits = (
        [sum(far[2 * far_entries + m * masks + i] << 32 * i for i in range(masks)) for m in range(2)]
        if masks
        else (0, 0)
    )
    mask, *words = PUSH_UNPACK(p, PIPELINE.size + packfile.PACK_ENTRIES * 8)
    *written_format, xr_slots = packfile.PIPELINE_V22.unpack_from(p, V22_AT)
    head, array_length_word, pulled, streams, viewport_count_word = packfile.PIPELINE_V21.unpack_from(p, V21_AT)
    stride_twin, layer_word, viewport_word = packfile.PIPELINE_V24.unpack_from(p, V24_AT)[2:5]
    guest_base_word, guest_base_slot = packfile.PIPELINE_V36.unpack_from(p, V36_AT)
    streams = streams[: streams.index(0)] if 0 in streams else streams
    tail = V20_AT - packfile.PIPELINE_V18.size - packfile.PIPELINE_V15.size
    half, _reserved = HALF_UNPACK(p, tail - 56)
    storage_mask, storage_write_mask, storage_samples, storage_tests, *storage_tic = STORAGE_UNPACK(p, tail - 52)
    depth_word, stencil_word, test_flags, shadow_word, depth_tic, stencil_tic, _reserved = TESTS_UNPACK(p, tail - 16)
    v15 = packfile.PIPELINE_V15.unpack_from(p, tail)
    _variants, vertex_variants, _fragment_variants, storage_blend_mask, storage_blend_model = v15[:5]
    bound_entries, uniform, fetch_rung, _variant_name = v15[5:]
    (memory_entries,) = packfile.PIPELINE_V18.unpack_from(p, tail + packfile.PIPELINE_V15.size)
    v20 = packfile.PIPELINE_V20.unpack_from(p, V20_AT)
    heap_arrays, heap_bytes, heap_words = v20[6], v20[7], v20[8:24]
    (spill_high,) = SPILL_HIGH_UNPACK(p, V22_AT + packfile.V22_SPILL_HIGH)
    spill_entry = [v20[24] | (spill_high & 0xFF) << 8, v20[25] | (spill_high >> 8) << 8]
    spill_words, spill = v20[26:28], rec.get('spill', [])
    regs = rec['probe']['records'][0]['regs']
    check_streams(rec['key'], slot, bool(head[3]), streams, regs)
    targets = rec['probe']['targets']
    if storage_mask or shadow_word or (flags & 256):
        methods = list(varying)
        # a shader-side stencil's program runs in the ordered interlock as a storage form's does
        if storage_mask or shadow_word:
            for method in [0x0210, 0x0754, 0x1224] + [0x1A00 + 4 * i for i in range(8) if storage_mask >> i & 1]:
                if method not in methods:
                    methods.append(method)
        if flags & 256 and 0x1224 not in methods:  # PipelineRasterOrdered: its interlock control is its own
            methods.append(0x1224)
        blob = [(m, regs.get(m, 0)) for m in methods if m not in PROGRAM_OFFSETS and not mkpack.foreign(m, targets)]
    else:
        if varying_by_targets is not None:
            methods = varying_by_targets.get(targets)
            if methods is None:
                methods = varying_by_targets[targets] = [
                    m for m in varying if m not in PROGRAM_OFFSETS and not mkpack.foreign(m, targets)
                ]
        else:
            methods = [m for m in varying if m not in PROGRAM_OFFSETS and not mkpack.foreign(m, targets)]
        blob = [(m, regs.get(m, 0)) for m in methods]
    extras = rec.get('extras')
    if extras:
        blob += extras
    far_bin = struct.pack(f'<{2 * far_entries}I', *far[: 2 * far_entries]) if far_entries else b''
    return {
        'index': rec['probe']['index'],
        'key': rec['key'],
        'form': rec['probe']['form'],
        'vs': prog_index[_pid(rec['programs'][0], pid_cache)],
        'fs': prog_index[_pid(rec['programs'][1], pid_cache)] if rec['programs'][1]['image'] else packfile.NO_PROGRAM,
        'blob': blob,
        'restore': rec.get('restore', []),
        'tess': [prog_index[_pid(t, pid_cache)] for t in rec.get('tess_programs', ())] or [0, 0],
        'runtime_head': head,
        'pulled_stride': pulled,
        'stride_twin': stride_twin,
        'amplification_layer_word': layer_word,
        'amplification_viewport_word': viewport_word,
        'guest_base_word': list(guest_base_word),
        'guest_base_slot': list(guest_base_slot),
        'push': [(0x700 + 4 * i, words[i]) for i in range(32) if mask >> i & 1] if mask else [],
        'push_metadata': bool(flags & 4),
        'turn_refused': bool(flags & 64),
        'raster_ordered': bool(flags & 256),
        'half_blend_color_word': half,
        'xr_slots': xr_slots,
        'storage_mask': storage_mask,
        'storage_write_mask': storage_write_mask,
        'storage_samples': storage_samples,
        'storage_tests': storage_tests,
        'storage_blend_mask': storage_blend_mask,
        'storage_blend_model': storage_blend_model,
        'depth_test_word': depth_word,
        'stencil_test_word': stencil_word,
        'test_flags': test_flags,
        'shadow_stencil_word': shadow_word,
        'depth_tic': depth_tic,
        'stencil_tic': stencil_tic,
        'storage_tic': storage_tic,
        'set_size': set_bytes,
        'entries': entries_of(p, entries, rec['samplers'], constexpr_tsc, far_bin, tsc_index),
        'vbs': [{'stream': 0, 'slot': slot, 'stride': stride}] if slot != 0xFF else [],
        'vertex_streams': list(streams),
        'array_length_word': array_length_word,
        'viewport_count_word': viewport_count_word,
        'fetch': bool(flags & 1),
        'clears': 0,
        'problems': [],
        'bound_entries': bound_entries | far_bits[0] << packfile.PACK_ENTRIES,
        'memory_entries': memory_entries | far_bits[1] << packfile.PACK_ENTRIES,
        'root_table_word': list(v20[0:2]),
        'status_word': list(v20[2:4]),
        'root_slots': list(v20[4:6]),
        'heap': [{'class': heap_words[2 * i], 'offset': heap_words[2 * i + 1]} for i in range(heap_arrays)],
        'heap_bytes': heap_bytes,
        'spill_entry': spill_entry,
        'spill': [spill[: spill_words[0]], spill[spill_words[0] : spill_words[0] + spill_words[1]]],
        'uniform': uniform,
        'fetch_rung': list(fetch_rung),
        'iterated_name': rec.get('iterated', ''),
        'fs_image': rec['programs'][1]['image'],
        'written_format': written_format,
        'variants': [
            {
                'stage': 0 if i < vertex_variants else 1,
                'assume': v['assume'],
                'program': prog_index[_pid(v['program'], pid_cache)],
                'memory': v['memory'],
            }
            for i, v in enumerate(rec.get('variants', ()))
        ],
    }


def init_records(path: str) -> list[tuple[int, int, int, int]]:
    """Give the context init as the pack holds it: each method's subchannel, method, word and relocation (I lines)."""
    with open(path) as f:
        rows = [line.split()[1:] for line in f if line.startswith('I ')]
    return [(int(sc), int(m, 16), int(d, 16), int(reloc)) for sc, m, d, reloc in rows]


def draw_context(post: dict[int, int]) -> dict[int, int]:
    """Give the registers a state's draw meets but does not set: post-init's, as draw_context.txt changes them."""
    context = dict(post)
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'draw_context.txt')) as f:
        for line in f:
            if not line.strip() or line.startswith('#'):
                continue
            method, value = line.split()
            if value == '-':
                context.pop(int(method, 16), None)
            else:
                context[int(method, 16)] = int(value, 16)
    return context


def gather(
    buckets: list[str], context: dict[int, int], pid_cache: dict[int, str] | None = None
) -> tuple[list[dict], list[dict]]:
    """Give the buckets' made states, each (form, key) once, their registers laid over `context`, and those refused."""
    states, seen, refused = [], set(), []
    for d in buckets:
        for rec in load_records(os.path.join(d, 'records.bin'))[0]:
            flags = PIPELINE.unpack_from(rec['pipeline'])[12] if rec['outcome'] == 0 else 0
            form = 'free' if flags & 32 else 'barrier' if flags & 2 else ''
            if (form, rec['key']) in seen:
                continue
            seen.add((form, rec['key']))
            if rec['outcome']:
                refused.append(rec)
            else:
                rec['form'] = form
                states.append(rec)
    split_identities(states, pid_cache)
    context_cache: dict[int, dict[int, int]] = {}
    for index, rec in enumerate(states):
        rec['probe'] = probe_of(rec, index, context, pid_cache=pid_cache, context_cache=context_cache)
    return states, refused


def probe_of(
    rec: dict,
    index: int,
    context: dict[int, int],
    *,
    pid_cache: dict[int, str] | None = None,
    context_cache: dict[int, dict[int, int]] | None = None,
) -> dict:
    """Give a made state as mkpack.py takes a probe's.

    Its registers are laid over `context`, less those of the targets its draw does not write (mkpack.targets).
    """
    n = mkpack.targets(rec['key'])
    if context_cache is not None:
        base = context_cache.get(n)
        if base is None:
            base = context_cache[n] = {
                m: 0 if mkpack.disabled_stream(m, v) else v for m, v in context.items() if not mkpack.foreign(m, n)
            }
        regs = dict(base)
        for m, v in rec['regs'].items():
            if not mkpack.foreign(m, n):
                regs[m] = 0 if mkpack.disabled_stream(m, v) else v
    else:
        regs = dict(context)
        regs.update(rec['regs'])
        regs = {m: 0 if mkpack.disabled_stream(m, v) else v for m, v in regs.items() if not mkpack.foreign(m, n)}
    programs = stage_programs(rec)
    ids = [_pid(program, pid_cache) for program in programs]
    for method, pid in zip(PROGRAM_OFFSETS, ids, strict=False):
        regs[method] = int(pid, 16) & 0xFFFFFFFF
    return {
        'index': index,
        'key': rec['key'],
        'form': rec['form'],
        'targets': n,
        'records': [{'regs': regs, 'prog': dict(zip((1, 5), ((i,) for i in ids), strict=False))}],
    }


def compute_kernels(
    buckets: list[str], constexpr_tsc: list[list[int]], laid: tuple, report: list[str]
) -> tuple[list[dict], list[dict]]:
    """Give the buckets' kernels (kernels.py) and answers, their programs appended to the heap image `laid` holds."""
    records = [rec for d in buckets for rec in load_records(os.path.join(d, 'records.bin'))[1]]
    programs = {}
    for rec in records:
        if rec['outcome'] == 0:
            rec['program'], program = program_id(rec['program']), rec['program']
            programs[rec['program']] = program
    computes, skipped = kernels.from_records(records, constexpr_tsc)
    kernels.lay_out(computes, programs, *laid)
    report.append(f'{len(computes)} compute kernels, {len(skipped)} dispatch shapes left out')
    report += [f'  skipped kernel {key}: {why}' for key, why in skipped]
    facts = kernels.state_facts(records)
    report.append(f'{len(facts)} compute states answered')
    return computes, facts


def split_identities(states: list[dict], pid_cache: dict[int, str] | None = None) -> None:
    """Keep distinct splits of identical whole programs addressable, with one shared whole-program heap slot."""
    variants: dict[str, set[bytes]] = {}
    identities = []
    for rec in states:
        whole = _pid(rec['programs'][1], pid_cache)
        split = rec.get('split')
        identity = (
            split['record'] + b''.join(bytes.fromhex(_pid(p, pid_cache).zfill(16)) for p in split['programs'])
            if split
            else b''
        )
        variants.setdefault(whole, set()).add(identity)
        identities.append((whole, identity))
    for rec, (whole, identity) in zip(states, identities, strict=True):
        if len(variants[whole]) > 1:
            old = rec['programs'][1]
            if pid_cache is not None:
                pid_cache.pop(id(old), None)
            rec['programs'][1] = dict(old, identity=identity, storage_id=whole)


def split_programs(
    states: list[dict], programs: dict[str, dict], pid_cache: dict[int, str] | None = None
) -> dict[str, dict]:
    """Give the states' splits by their whole program's id; their per-pixel programs and prologues join `programs`."""
    found = {}
    for rec in states:
        if 'split' not in rec:
            continue
        fs, prologue = rec['split']['programs']
        ids = [_pid(fs, pid_cache), _pid(prologue, pid_cache)]
        programs.update(zip(ids, (fs, prologue), strict=True))
        found[_pid(rec['programs'][1], pid_cache)] = splits.of_record(rec['split']['record'], *ids)
    return found


def lay_out_variants(
    states: list[dict],
    programs: dict[str, dict],
    laid: tuple,
    report: list[str],
    pid_cache: dict[int, str] | None = None,
) -> None:
    """Append the states' variant and tessellation programs to the heap image `laid` holds, after every generic one."""
    count = tessellating = 0
    for rec in states:
        variants = rec.get('variants')
        if variants:
            count += len(variants)
            for v in variants:
                p = v['program']
                pid = _pid(p, pid_cache)
                programs.setdefault(pid, p)
                packfile.place_graphics(pid, programs, *laid)
        tess = rec.get('tess_programs')
        if tess:
            tessellating += 1
            for p in tess:
                pid = _pid(p, pid_cache)
                programs.setdefault(pid, p)
                packfile.place_graphics(pid, programs, *laid)
    report.append(f'{count} program variants, {tessellating} tessellation states')


def write_programs(path: str, programs: dict[str, dict]) -> None:
    """Write the programs as mkprograms.py does (nvdec names a bound program by them: test/eq.sh RAWCAP_PROGRAMS)."""
    with open(path, 'wb') as f:
        for p in programs.values():
            f.write(b'NVPG' + struct.pack('<III', len(p['image']) - 80, len(p['cbufs']), len(p['data'])))
            f.write(bytes(p['cbufs']) + p['image'] + p['data'])


# The forms a variant record's name holds (runtime/pack.h variantNameAt), as the report counts them
VARIANT_FORMS = (
    ('tests=', 'of a test class'),
    ('cubic', 'of cubic samplers'),
    ('xrs=', 'of sampled two-plane textures'),
    ('wtex=', 'of written formats'),
)


def variant_report(states: list[dict]) -> list[str]:
    """Give the report's count of variant records by form, then each sampled two-plane variant a corpus line named."""
    named = [packfile.variant_name(rec['key']) for rec in states]
    counts = ', '.join(f'{sum(1 for n in named if tag in n)} {what}' for tag, what in VARIANT_FORMS)
    sampled = dict.fromkeys(
        (n, rec['key']) for rec, n in zip(states, named, strict=True) if 'xrs=' in n and 'tests=' not in n
    )
    return [f'{sum(1 for n in named if n)} variant records: {counts}'] + [
        f'  sampled two-plane variant ({n.removeprefix(" | ")}): {key[:150]}' for n, key in sampled
    ]


def read_option(buckets: list[str], name: str) -> str | None:
    if name not in buckets:
        return None
    at = buckets.index(name)
    with open(buckets[at + 1]) as f:
        text = f.read()
    del buckets[at : at + 2]
    return text


def read_reflections(buckets: list[str], report: list[str]) -> dict[tuple[bool, int], bytes]:
    reflections: dict[tuple[bool, int], bytes] = {}
    for d in buckets:
        for state, blob in read_records(os.path.join(d, 'records.bin'))[2].items():
            reflections.setdefault(state, blob)
    report.append(
        f'{sum(not c for c, _ in reflections)} render and {sum(c for c, _ in reflections)} compute reflections, '
        f'{len(set(reflections.values()))} distinct'
    )
    return reflections


def write_outputs(
    out_dir: str,
    pack: packfile.Pack,
    varying: list[int],
    programs: dict[str, dict],
    phase: Callable[[str], None],
    hashes: list[int] | None = None,
) -> tuple[bytes, list[int]]:
    data, blob = packfile.pack_bytes(pack, hashes=hashes)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'pack.bin'), 'wb') as f:
        f.write(data)
    phase('bytes')
    packfile.write_json(os.path.join(out_dir, 'pack.json'), pack, varying)
    phase('json')
    packfile.write_layouts(os.path.join(out_dir, 'layouts.txt'), pack)
    write_programs(os.path.join(out_dir, 'programs.bin'), programs)
    phase('files')
    return data, blob


def main() -> None:
    out_dir, *buckets = sys.argv[1:]
    # Each step's seconds, printed (not in report.txt, which must not differ between two links of the same buckets)
    phases: dict[str, float] = {}
    since = [time.monotonic()]

    def phase(name: str) -> None:
        now = time.monotonic()
        phases[name] = now - since[0]
        since[0] = now

    pid_cache: dict[int, str] = {}
    key_hashes: dict[int, int] = {}
    inventory = read_option(buckets, '--archives') or ''
    warm = next(d for d in buckets if os.path.exists(os.path.join(d, 'context.log')))
    _, post = mkpack.load_init(os.path.join(warm, 'context.log'))
    states, refused = gather(buckets, draw_context(post[0]), pid_cache)
    fast, tiers = fast_keys([line for d in buckets for line in read_records(os.path.join(d, 'records.bin'))[3]])
    set_tiers(states, tiers, key_hashes)
    phase('gather')
    programs = {_pid(p, pid_cache): p for rec in states for p in stage_programs(rec)}
    found = split_programs(states, programs, pid_cache)
    simple = [rec['probe'] for rec in states]
    report = [
        f'{len(states) + len(refused)} states made directly, {len(states)} records, {len(refused)} refused',
        *variant_report(states),
    ]
    phase('programs')
    varying, baseline = mkpack.split_registers(simple, post)
    report.append(f'{len(varying)} per-pipeline registers: ' + ' '.join(f'{m:x}' for m in varying))
    report.append(f'{len(baseline)} baseline registers over post-init')
    phase('registers')
    laid, split, tier0_programs, tier0_heap = lay_out_tiers(states, programs, found, report, pid_cache)
    heap, prog_index, prog_recs = laid
    phase('tiers')
    constexpr_tsc: list[list[int]] = []
    tsc_index: dict[tuple[int, ...], int] = {}
    varying_by_targets: dict[int, list[int]] = {}
    pipe_hashes: dict[int, int] = {}
    pipes = []
    for rec in states:
        pipe = pipeline_of(
            rec,
            varying,
            prog_index,
            constexpr_tsc,
            pid_cache=pid_cache,
            tsc_index=tsc_index,
            varying_by_targets=varying_by_targets,
        )
        pipe['tier'] = rec['tier']
        pipe_hashes[id(pipe)] = key_hashes[id(rec)]
        pipes.append(pipe)
    pipes.sort(key=lambda rec: (rec['tier'], pipe_hashes[id(rec)]))
    phase('pipelines')
    pipes, iterated = fetchfree_link.attach(pipes, report)
    phase('fetchfree')
    computes, compute_facts = compute_kernels(buckets, constexpr_tsc, laid, report)
    phase('kernels')
    init_recs = init_records(os.path.join(warm, 'context.log'))
    declared, kinds = archives.declared(inventory, archives.refused_keys(states, refused))
    held = archives.held_kernels([k['key'] for k in computes])
    report.append(archives.report(declared, kinds, held))
    reflections = read_reflections(buckets, report)
    phase('reflections')
    pack = packfile.Pack(
        pipes,
        prog_recs,
        constexpr_tsc,
        baseline,
        init_recs,
        heap,
        computes,
        split,
        archives.section(declared, held),
        iterated,
        compute_facts,
        reflections,
        fast,
        tier0_programs,
        tier0_heap,
    )
    segments.build_tier0_segments(pack, report)
    phase('segments')
    data, blob = write_outputs(out_dir, pack, varying, programs, phase, hashes=[pipe_hashes[id(r)] for r in pack.pipes])
    unshared = sum(len(p['blob']) * 2 for p in pipes)
    report.append(
        f'pack.bin {len(data) // 1024} KiB: {len(pipes)} pipelines, {len(computes)} kernels, '
        f'{len(prog_recs)} programs, {len(split)} splits, {len(constexpr_tsc)} constexpr samplers, '
        f'{len(blob)} blob dwords ({unshared} unshared), {len(baseline)} baseline regs'
    )
    report.append(packfile.sections_line(data))
    report += [f'  refused ({rec["why"]}): {rec["key"][:150]}' for rec in refused]
    with open(os.path.join(out_dir, 'report.txt'), 'w') as f:
        f.write('\n'.join(report) + '\n')
    print('\n'.join(report))  # the refused states too: a state the pack drops is named in the build's log
    print('link seconds: ' + ', '.join(f'{name} {s:.1f}' for name, s in phases.items()))


if __name__ == '__main__':
    main()
