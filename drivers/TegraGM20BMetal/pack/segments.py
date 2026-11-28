"""Pre-built GM20B state segments and tier-0 local-memory ceiling (pack v34; v35: the words block in the table; v37:
FLAG_BLEND_KILL).

Each tier-0 pipeline record (and each reachable program-variant combination of it) gets its immutable
3D method stream written once into tier 0 of PackHeap, 16-byte aligned and deduplicated, so the
runtime binds it straight out of the shared read-only pack window with zero locks, copies or kernel
calls. Constant-buffer binds for CbufShaderData use TGM_PACK_WINDOW_VA + heap_offset + data_offset
and set FLAG_RESELECT_ROOT so RenderEncoder::bindState re-selects the per-process root table.
"""

from __future__ import annotations

import array
import functools
import os
import re
import struct
import sys

import packfile

PACK_WINDOW_VA = (1 << 39) - (4 << 30)  # TegraGM20BABI.h TGM_PACK_WINDOW_VA
TIER_ALIGN = packfile.TIER_ALIGN
PREFETCH = 0x1000  # program_pages.h kProgramPrefetchBytes

SEGMENT_BIND_WORDS = 8  # state_segment.h kSegmentBindWords
SEGMENT_MAX_WORDS = 0xFFFF

FLAG_KEPT_ACROSS_PASSES = 1 << 0
FLAG_RESELECT_ROOT = 1 << 1
# The segment's last SET_ITERATED_BLEND_OPTIMIZATION is NOOP_SOURCE_RGBA_0000: what the GPU runs, which a runtime reads
# from the wired segment table instead of the segment's heap words
FLAG_BLEND_KILL = 1 << 2

CBUF_SHADER_DATA = 2
CBUF_UNIT = 256
CBUF_MAX_BYTES = 0x10000

SET_GROUPS = (0, 4, 1, 2)  # vertex, pixel, tessellation init, tessellation

CT_WRITE = 0x1A00
PIXEL_SHADER_INTERLOCK_CONTROL = 0x1224
HYBRID_ANTI_ALIAS_CONTROL = 0x0754
ITERATED_BLEND_OPTIMIZATION = 0x02F4
NOOP_NEVER = 0
NOOP_SOURCE_RGBA_0000 = 1

SET_PIPELINE_PROGRAM_VS = 0x2044
SET_PIPELINE_REGISTER_COUNT_VS = 0x204C
SET_PIPELINE_PROGRAM_FS = 0x2144
SET_PIPELINE_REGISTER_COUNT_FS = 0x214C
SET_PIPELINE_PROGRAM_TESS = 0x2084
SET_PIPELINE_REGISTER_COUNT_TESS = 0x208C
PIPELINE_STAGE_STRIDE = 0x40
SET_PATCH = 0x0DCC
SET_BINDLESS_TEXTURE = 0x2608
CB_SELECTOR_A = 0x2380
BIND_GROUP_CB = 0x2410

PIXEL_STAGE_SWITCH = 0x2140
PIXEL_STAGE_OFF = 0x50
RASTER_SWITCH = 0x037C

SWIZZLE_METHOD = 0x0A18
SWIZZLE_VIEWPORTS = 16

FETCH_BLEND = 2
FETCH_ITERATED_BLEND = 3
FETCH_RUNG_MASK = 0x0F

NORMALIZED_FORMATS = frozenset(
    (1, 10, 11, 12, 20, 22, 30, 31, 32, 40, 41, 42, 43, 60, 62, 70, 71, 72, 80, 81, 90, 94, 110, 112)
)
NOOP_BLEND_OPS = frozenset((0x8006, 0x800B))
NOOP_DEST_COEFFS = frozenset((0x4001, 0x4301, 0x4303))

_PASS_WRITE_RANGES = (
    (0x0DC0, 0x0DC4),
    (0x0DC4, 0x0DC8),
    (0x0DC8, 0x0DCC),
    (0x15BC, 0x15C0),
    (0x156C, 0x1570),
    (0x187C, 0x1880),
    (0x1918, 0x191C),
    (0x191C, 0x1920),
    (0x1920, 0x1924),
    (0x0DAC, 0x0DB0),
    (0x0DB0, 0x0DB4),
    (0x1438, 0x143C),
    (0x1434, 0x1438),
    (0x1118, 0x111C),
    (0x0A18, 0x0C18),
    (0x19E0, 0x1A00),
    (0x0D80, 0x0D90),
    (0x0D6C, 0x0D74),
    (0x10F8, 0x10FC),
    (0x1944, 0x1948),
    (0x0D90, 0x0D94),
    (0x0DA0, 0x0DA4),
    (0x1538, 0x153C),
    (0x179C, 0x17A0),
    (0x19CC, 0x19D0),
    (0x111C, 0x1120),
    (0x1208, 0x120C),
    (0x0370, 0x0374),
    (0x02E8, 0x02EC),
    (0x02F8, 0x0300),
    (0x07C0, 0x07F8),
    (0x0DBC, 0x0DC0),
    (0x0DD8, 0x0DDC),
    (0x1590, 0x1594),
    (0x15C8, 0x15CC),
    (0x15FC, 0x1604),
)
PASS_WRITE_METHODS = frozenset(m for lo, hi in _PASS_WRITE_RANGES for m in range(lo, hi, 4))

_SEGMENT_TABLE = struct.Struct('<IIII')  # pack.h PackSegmentTable
_SEGMENT_ENTRY = struct.Struct('<IHH')
_U32 = struct.Struct('<I')
_SPH_HEAD = struct.Struct('<4I')


@functools.cache
def _dropped_methods() -> frozenset[int]:
    """Give owned.h methods plus swizzle methods dropped from pipeline blobs."""
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'owned.h')) as header:
        text = header.read()
    pair = r'\{(0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)\}'
    table = text[text.index('kOwnedMethods[]') : text.index('};', text.index('kOwnedMethods[]'))]
    streams = re.search(r'kVertexStreams = ' + pair, text)
    assert streams is not None
    dropped: set[int] = set()
    for a, b in re.findall(pair, table):
        dropped.update(range(int(a, 16), int(b, 16), 4))
    for m in range(int(streams[1], 16), int(streams[2], 16), 4):
        if (m & 0xF) in (4, 8):
            dropped.add(m)
    for j in range(SWIZZLE_VIEWPORTS):
        dropped.add(SWIZZLE_METHOD + 32 * j)
    return frozenset(dropped)


def _hdr(op: int, mthd: int, count: int) -> int:
    return (op << 29) | (count << 16) | (mthd >> 2)


def _push_m(out: list[int], mthd: int, val: int) -> None:
    if 0 <= val < 0x2000:
        out.append((4 << 29) | (val << 16) | (mthd >> 2))
    else:
        out.append((1 << 29) | (1 << 16) | (mthd >> 2))
        out.append(val & 0xFFFFFFFF)


def _push_inc(out: list[int], mthd: int, vals: tuple[int, ...] | list[int]) -> None:
    out.append((1 << 29) | (len(vals) << 16) | (mthd >> 2))
    out.extend(vals)


def _push_0inc(out: list[int], mthd: int, vals: list[int]) -> None:
    out.append((3 << 29) | (len(vals) << 16) | (mthd >> 2))
    out.extend(vals)


def _last_written(words: list[int], method: int, absent: int) -> int:
    """The value the last write of 3D `method` in `words` leaves it, `absent` where none writes it (push.h lastWritten)."""
    value = absent
    i = 0
    while i < len(words):
        hdr = words[i]
        op, arg, mthd, mine = hdr >> 29, hdr >> 16 & 0x1FFF, (hdr & 0xFFF) << 2, (hdr >> 13 & 7) == 0
        if op == 4:
            if mine and mthd == method:
                value = arg
            i += 1
            continue
        for k in range(arg):
            if i + 1 + k >= len(words):
                break
            at = mthd + 4 * k if op == 1 else mthd + 4 if op == 5 and k else mthd
            if mine and at == method and op in (1, 3, 5):
                value = words[i + 1 + k]
        i += 1 + arg if op in (1, 3, 5) else 1
    return value


def _cbuf_bind_bytes(size: int) -> int:
    return min((size + CBUF_UNIT - 1) & ~(CBUF_UNIT - 1), CBUF_MAX_BYTES)


def _legal_interlock(interlock: int, hybrid: int) -> int:
    if (hybrid & 0xF) > 1 and (interlock & 3) == 2:
        return (interlock & ~3) | 1
    return interlock


def _key_color_format(key_string: str, target: int) -> int:
    tag = f' c{target}='
    at = key_string.find(tag)
    if at < 0:
        return 0
    pos = at + len(tag)
    end = pos
    n = len(key_string)
    while end < n and key_string[end].isdigit():
        end += 1
    return int(key_string[pos:end]) if end > pos else 0


def _color_formats(key_string: str) -> list[int]:
    formats = [0] * 8
    start = 0
    while True:
        at = key_string.find(' c', start)
        if at < 0:
            break
        start = at + 2
        eq = key_string.find('=', start)
        if eq <= start:
            continue
        idx_s = key_string[start:eq]
        if not idx_s.isdigit():
            continue
        c = int(idx_s)
        if c >= 8:
            continue
        sp = key_string.find(' ', eq + 1)
        tail = key_string[eq + 1 : sp if sp >= 0 else len(key_string)]
        parts = tail.split(':')
        if len(parts) >= 4 and all(p.isdigit() for p in parts[:4]):
            fmt = int(parts[0])
            formats[c] = 115 if (fmt == 542 or 550 <= fmt <= 555) else fmt
    return formats


def _noop_source_coeff(c: int, normalized: bool) -> bool:
    if c in (0x4000, 0x4001) or 0x4300 <= c <= 0x4303:
        return True
    return normalized and 0x4304 <= c <= 0x4308


def _iterated_blend_noop(
    blob: list[tuple[int, int]],
    writes: list[int],
    key_string: str,
    storage_bits: int,
    default_per_target: list[int] | None = None,
    default_seen: int = 0,
) -> int:
    if not (writes[0] & 0x1111) or (storage_bits & 1) or not key_string:
        return NOOP_NEVER
    # only the key's targets: the baseline's SET_CT_WRITE of a target the state lacks is no attachment of its passes
    for j in range(1, 8):
        if writes[j] & 0x1111 and _key_color_format(key_string, j):
            return NOOP_NEVER
    blends = False
    per_target = list(default_per_target) if default_per_target is not None else [0] * 6
    seen = default_seen if default_per_target is not None else 0
    for m, v in blob:
        if m == 0x153C and (v & 0x11):
            return NOOP_NEVER
        if m == 0x1360:
            blends = bool(v & 1)
        elif 0x1E04 <= m <= 0x1E18:
            idx = (m - 0x1E04) >> 2
            per_target[idx] = v
            seen |= 1 << idx
    if seen != 0x3F or not blends:
        return NOOP_NEVER
    normalized = _key_color_format(key_string, 0) in NORMALIZED_FORMATS
    for half in (0, 3):
        if (
            per_target[half] not in NOOP_BLEND_OPS
            or not _noop_source_coeff(per_target[half + 1], normalized)
            or per_target[half + 2] not in NOOP_DEST_COEFFS
        ):
            return NOOP_NEVER
    return NOOP_SOURCE_RGBA_0000


def _bind_program_cbufs(out: list[int], prog: dict, group: int) -> bool:
    reselect = False
    cbufs = prog['cbufs']
    data_bytes = prog.get('data_bytes', 0)
    data_va = (PACK_WINDOW_VA + prog['heap_offset'] + prog['data_offset']) if data_bytes else 0
    bind_size = _cbuf_bind_bytes(data_bytes)
    hi = (data_va >> 32) & 0xFFFFFFFF
    lo = data_va & 0xFFFFFFFF
    bind_mthd = BIND_GROUP_CB + 0x20 * group
    for c in range(1, len(cbufs)):
        if cbufs[c] != CBUF_SHADER_DATA:
            continue
        _push_inc(out, CB_SELECTOR_A, (bind_size, hi, lo))
        _push_m(out, bind_mthd, 1 | (c << 4))
        reselect = True
    return reselect


def _fetch_free_iterated(rec: dict, iterated_list: list[dict]) -> tuple[int, dict] | None:
    if rec.get('form') != 'free':
        return None
    fetch_rung = rec.get('fetch_rung')
    if not fetch_rung:
        return None
    it_idx = rec.get('iterated', 0)
    it = iterated_list[it_idx - 1] if 1 <= it_idx <= len(iterated_list) else None
    for c in range(min(8, len(fetch_rung))):
        rung = fetch_rung[c] & FETCH_RUNG_MASK
        if rung not in (FETCH_BLEND, FETCH_ITERATED_BLEND):
            continue
        if rung != FETCH_ITERATED_BLEND:
            return None
        if it is None:
            return None
        prog_words = it['words']
        matrix = len(prog_words) == 0 and it.get('uniform_slot', 0xFF) < 31
        if not matrix and (len(prog_words) == 0 or len(prog_words) > 128):
            return None
        if matrix and not (10 <= it.get('uniform_words', 0) <= 11):
            return None
        return c, it
    return None


def _emit_iterated_blend(out: list[int], it: dict, colour: int, fmt: int) -> None:
    out.append(_hdr(1, 0x1360 + 4 * colour, 1))
    out.append(1)
    if fmt in (70, 80):
        out.append(_hdr(1, 0x13A8, 1))
        out.append(1 << (4 * colour))
    _push_m(out, 0x12E4, 0)
    _push_inc(out, 0x133C, (1, 0x8006, 0x4001, 0x4000, 0x8006, it['alpha_source']))
    _push_m(out, 0x1358, it['alpha_dest'])
    prog_words = it['words']
    if prog_words:
        consts = it['constants']
        for j in range(len(consts)):
            _push_inc(out, 0x1480 + 16 * j, consts.get(j, (0, 0, 0)))
        _push_m(out, 0x02F4, 0)
        _push_m(out, 0x0DF0, 0)
        _push_0inc(out, 0x0DF4, prog_words)
        _push_m(out, 0x0DD4, len(prog_words))
        _push_m(out, 0x0DD0, 1 | (2 if (it['enable'] & 2) else 0))


def _choose_variant(variants: list[dict], facts: int) -> int:
    for i, v in enumerate(variants):
        if not (v['assume'] & ~facts):
            return i
    return len(variants)


def _tessellates(rec: dict) -> bool:
    head = rec.get('runtime_head')
    return bool(head and head[0])


def slm_per_warp_of_header(heap: bytes | bytearray, prog_offset: int) -> int:
    if prog_offset + 16 > len(heap):
        return 0
    _sph0, sph1, sph2, sph3 = _SPH_HEAD.unpack_from(heap, prog_offset)
    per_thread = (sph1 & 0xFFFFFF) | ((sph2 & 0xFFFFFF) << 32)
    crs = sph3 & 0xFFFFFF
    if not per_thread and not crs:
        return 0
    return (per_thread * 32 + crs + 0x1FF) & ~0x1FF


def build_pipeline_stream(
    rec: dict,
    vs: dict,
    fs: dict | None,
    base_ct_write: list[int],
    iterated_list: list[dict],
    tess: tuple[dict, dict] | None,
    dropped: frozenset[int],
    base_blend: list[int] | None = None,
    base_blend_seen: int = 0,
) -> tuple[list[int], int]:
    """Build a pipeline's GM20B state-segment method words and PackSegmentFlag mask."""
    key = rec['key']
    bar = key.find(' | ')
    key_string = (key[:bar] if bar >= 0 else key).rstrip()
    words: list[int] = []
    writes = list(base_ct_write)
    interlock = 0
    sample_control = 1
    kept = True
    pass_writes = PASS_WRITE_METHODS
    blob = rec['blob']
    for m, v in blob:
        if m in dropped:
            continue
        if CT_WRITE <= m < CT_WRITE + 32:
            writes[(m - CT_WRITE) >> 2] = v
        if m == PIXEL_SHADER_INTERLOCK_CONTROL:
            interlock = v
            continue
        if m == HYBRID_ANTI_ALIAS_CONTROL:
            sample_control = v
            continue
        if kept and m in pass_writes:
            kept = False
        _push_m(words, m, v)
    _push_m(words, PIXEL_SHADER_INTERLOCK_CONTROL, _legal_interlock(interlock, sample_control))
    _push_m(words, HYBRID_ANTI_ALIAS_CONTROL, sample_control)
    storage_mask = rec.get('storage_mask', 0)
    blend_kill = _iterated_blend_noop(
        blob, writes, key_string, storage_mask if storage_mask else 0, base_blend, base_blend_seen
    )
    _push_m(words, ITERATED_BLEND_OPTIMIZATION, blend_kill)
    _push_m(words, SET_PIPELINE_PROGRAM_VS, vs['program'])
    _push_m(words, SET_PIPELINE_REGISTER_COUNT_VS, vs.get('gprs', 0))
    if fs is not None:
        _push_m(words, SET_PIPELINE_PROGRAM_FS, fs['program'])
        _push_m(words, SET_PIPELINE_REGISTER_COUNT_FS, fs.get('gprs', 0))
        bound_tex = fs.get('bound_textures', 0)
        if bound_tex:
            _push_m(words, SET_BINDLESS_TEXTURE, bound_tex - 1)
    reselect = _bind_program_cbufs(words, vs, SET_GROUPS[0])
    if fs is not None:
        reselect |= _bind_program_cbufs(words, fs, SET_GROUPS[1])
    if tess is not None:
        _push_m(words, SET_PATCH, 1)
        for k, tp in enumerate(tess):
            _push_m(words, SET_PIPELINE_PROGRAM_TESS + PIPELINE_STAGE_STRIDE * k, tp['program'])
            _push_m(words, SET_PIPELINE_REGISTER_COUNT_TESS + PIPELINE_STAGE_STRIDE * k, tp.get('gprs', 0))
            reselect |= _bind_program_cbufs(words, tp, SET_GROUPS[2 + k])
    if fs is None:
        _push_m(words, PIXEL_STAGE_SWITCH, PIXEL_STAGE_OFF)
    if ' rast=0' in key_string:
        _push_m(words, RASTER_SWITCH, 0)
    free_it = _fetch_free_iterated(rec, iterated_list)
    if free_it is not None:
        colour, it = free_it
        _emit_iterated_blend(words, it, colour, _color_formats(key_string)[colour])
    kills = _last_written(words, ITERATED_BLEND_OPTIMIZATION, NOOP_NEVER) == NOOP_SOURCE_RGBA_0000
    flags = (
        (FLAG_KEPT_ACROSS_PASSES if kept else 0)
        | (FLAG_RESELECT_ROOT if reselect else 0)
        | (FLAG_BLEND_KILL if kills else 0)
    )
    return words, flags


def _words_to_bytes(words: list[int]) -> bytes:
    if sys.byteorder == 'little' and array.array('I').itemsize == 4:
        return array.array('I', words).tobytes()
    return struct.pack(f'<{len(words)}I', *words)


def encode_table(
    base: list[tuple[int, int, int]],
    var_bases: list[int],
    var_combos: list[tuple[int, int, int]],
    words: tuple[int, int] = (0, 0),
) -> bytes:
    """Encode PackSegments section: header, base[N], variantBase[N], variantCombos[M].

    `words`: the PackHeap block (offset, bytes) every segment's words lie in.
    """
    out = bytearray(_SEGMENT_TABLE.size + len(base) * (_SEGMENT_ENTRY.size + 4) + len(var_combos) * _SEGMENT_ENTRY.size)
    _SEGMENT_TABLE.pack_into(out, 0, len(base), len(var_combos), *words)
    at = _SEGMENT_TABLE.size
    pack_entry = _SEGMENT_ENTRY.pack_into
    for heap_off, count, flags in base:
        pack_entry(out, at, heap_off, count, flags)
        at += _SEGMENT_ENTRY.size
    pack_u32 = _U32.pack_into
    for vb in var_bases:
        pack_u32(out, at, vb)
        at += 4
    for heap_off, count, flags in var_combos:
        pack_entry(out, at, heap_off, count, flags)
        at += _SEGMENT_ENTRY.size
    return bytes(out)


def build_tier0_segments(pack: packfile.Pack, report: list[str] | None = None) -> None:
    """Pre-build tier-0 state segments into tier 0 of pack.heap and compute pack.tier0_slm_per_warp."""
    tier0_pipes = sum(1 for rec in pack.pipes if rec.get('tier', 0) == 0)
    tier0_progs = len(pack.programs) if pack.tier0_programs is None else pack.tier0_programs
    if not tier0_pipes or not tier0_progs:
        pack.segments = b''
        pack.tier0_slm_per_warp = 0
        return

    base_ct_write = [0] * 8
    base_blend = [0] * 6
    base_blend_seen = 0
    for sc, m, d, _ in pack.init:
        if sc == 0 and 0x1E04 <= m <= 0x1E18:
            idx = (m - 0x1E04) >> 2
            base_blend[idx] = d
            base_blend_seen |= 1 << idx
    for m, v in pack.baseline.items():
        if CT_WRITE <= m < CT_WRITE + 32:
            base_ct_write[(m - CT_WRITE) >> 2] = v
        elif 0x1E04 <= m <= 0x1E18:
            idx = (m - 0x1E04) >> 2
            base_blend[idx] = v
            base_blend_seen |= 1 << idx

    dropped = _dropped_methods()
    facts_base = packfile.chip_facts()
    programs = pack.programs
    seg_blob = bytearray()
    seg_pool: dict[bytes, int] = {}

    def place_stream(words: list[int], flags: int) -> tuple[int, int, int]:
        count = len(words)
        if count <= SEGMENT_BIND_WORDS or count > SEGMENT_MAX_WORDS:
            return (0, 0, 0)
        raw = _words_to_bytes(words)
        off = seg_pool.get(raw)
        if off is None:
            off = seg_pool[raw] = len(seg_blob)
            seg_blob.extend(raw)
            pad = (-len(seg_blob)) & 15
            if pad:
                seg_blob.extend(bytes(pad))
        return (off, count, flags)

    base_entries: list[tuple[int, int, int]] = []
    var_bases: list[int] = []
    var_combos: list[tuple[int, int, int]] = []
    gfx_prog_indices: set[int] = set()

    for rec in pack.pipes[:tier0_pipes]:
        vs_idx = rec['vs']
        fs_idx = rec['fs']
        if vs_idx >= tier0_progs or (fs_idx != packfile.NO_PROGRAM and fs_idx >= tier0_progs):
            base_entries.append((0, 0, 0))
            var_bases.append(0xFFFFFFFF)
            continue
        vs = programs[vs_idx]
        fs = None if fs_idx == packfile.NO_PROGRAM else programs[fs_idx]
        gfx_prog_indices.add(vs_idx)
        if fs_idx != packfile.NO_PROGRAM:
            gfx_prog_indices.add(fs_idx)
        tess: tuple[dict, dict] | None = None
        if _tessellates(rec):
            t0, t1 = rec['tess']
            if t0 >= tier0_progs or t1 >= tier0_progs:
                base_entries.append((0, 0, 0))
                var_bases.append(0xFFFFFFFF)
                continue
            tess = (programs[t0], programs[t1])
            gfx_prog_indices.add(t0)
            gfx_prog_indices.add(t1)

        words, flags = build_pipeline_stream(
            rec, vs, fs, base_ct_write, pack.iterated, tess, dropped, base_blend, base_blend_seen
        )
        base_entries.append(place_stream(words, flags))

        variants = rec.get('variants')
        if not variants:
            var_bases.append(0xFFFFFFFF)
            continue
        v0 = [v for v in variants if v['stage'] == 0]
        v1 = [v for v in variants if v['stage'] == 1]
        for v in variants:
            if v['program'] < tier0_progs:
                gfx_prog_indices.add(v['program'])
        c0, c1 = len(v0), len(v1)
        if not c0 and not c1:
            var_bases.append(0xFFFFFFFF)
            continue
        var_bases.append(len(var_combos))
        for vi in range(c0 + 1):
            for fi in range(c1 + 1):
                if vi == c0 and fi == c1:
                    continue
                facts = facts_base | (v0[vi]['assume'] if vi < c0 else 0) | (v1[fi]['assume'] if fi < c1 else 0)
                if _choose_variant(v0, facts) != vi or _choose_variant(v1, facts) != fi:
                    var_combos.append((0, 0, 0))
                    continue
                p0_idx = v0[vi]['program'] if vi < c0 else vs_idx
                p1_idx = v1[fi]['program'] if fi < c1 else fs_idx
                if p0_idx >= tier0_progs or (p1_idx != packfile.NO_PROGRAM and p1_idx >= tier0_progs):
                    var_combos.append((0, 0, 0))
                    continue
                p0 = programs[p0_idx]
                p1 = None if p1_idx == packfile.NO_PROGRAM else programs[p1_idx]
                vw, vf = build_pipeline_stream(
                    rec, p0, p1, base_ct_write, pack.iterated, None, dropped, base_blend, base_blend_seen
                )
                var_combos.append(place_stream(vw, vf))

    max_slm = 0
    for idx in gfx_prog_indices:
        max_slm = max(max_slm, slm_per_warp_of_header(pack.heap, programs[idx]['program']))
    pack.tier0_slm_per_warp = max_slm

    old_tier0_heap = len(pack.heap) if pack.tier0_heap is None else pack.tier0_heap
    words = (0, 0)
    if seg_blob:
        tier0_prog_end = max((pr['heap_offset'] + pr['bytes'] for pr in programs[:tier0_progs]), default=0)
        seg_start = (tier0_prog_end + PREFETCH + 15) & ~15
        seg_end = seg_start + len(seg_blob)
        words = (seg_start, len(seg_blob))
        new_tier0_heap = max(old_tier0_heap, (seg_end + TIER_ALIGN - 1) & ~(TIER_ALIGN - 1))
        delta = new_tier0_heap - old_tier0_heap
        if delta == 0:
            if len(pack.heap) < new_tier0_heap:
                pack.heap.extend(bytes(new_tier0_heap - len(pack.heap)))
            pack.heap[seg_start:seg_end] = seg_blob
        else:
            tier1_tail = bytes(pack.heap[old_tier0_heap:])
            new_heap = bytearray(pack.heap[:seg_start])
            if len(new_heap) < seg_start:
                new_heap.extend(bytes(seg_start - len(new_heap)))
            new_heap.extend(seg_blob)
            new_heap.extend(bytes(new_tier0_heap - len(new_heap)))
            new_heap.extend(tier1_tail)
            pack.heap = new_heap
            if pack.tier0_heap is not None:
                pack.tier0_heap = new_tier0_heap
            shifted: set[int] = set()
            for pr in programs:
                if id(pr) not in shifted and pr['heap_offset'] >= old_tier0_heap:
                    shifted.add(id(pr))
                    pr['heap_offset'] += delta
                    pr['program'] += delta
        base_entries = [(off + seg_start, n, f) if n else (0, 0, 0) for off, n, f in base_entries]
        var_combos = [(off + seg_start, n, f) if n else (0, 0, 0) for off, n, f in var_combos]

    pack.segments = encode_table(base_entries, var_bases, var_combos, words)
    if report is not None:
        report.append(
            f'tier 0 state segments: {len(base_entries)} pipelines, {sum(1 for _, n, _ in var_combos if n)} variant '
            f'combos, {len(seg_pool)} distinct ({len(seg_blob) // 1024} KiB), tier0 slm {max_slm} B/warp'
        )
