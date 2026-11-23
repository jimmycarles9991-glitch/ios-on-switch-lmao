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

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'iterated'))
import archives
import fetchfree_link
import kernels
import mkpack
import packfile
import selector_section
import splits

PIPELINE = struct.Struct('<QIIIIIBBBBHHI')
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
)
# Where each record tail starts in a PackPipeline
V24_AT = PIPELINE_BYTES - packfile.PIPELINE_V24.size
V22_AT = V24_AT - packfile.PIPELINE_V22.size
V21_AT = V22_AT - packfile.PIPELINE_V21.size
V20_AT = V21_AT - packfile.PIPELINE_V20.size
ENTRY_UBO, ENTRY_COLOR_TABLE = 1, 5
TABLE_KINDS = {code: kind for kind, code in packfile.TABLE_KINDS.items()}
# SET_VERTEX_STREAM_A_FORMAT(0), 16 bytes apart, and its STRIDE field (bits 11:0)
STREAM_FORMAT, STREAM_STRIDE = 0x1C00, 0xFFF
TEXTURE_KINDS = {2: 'slot', 3: 'fetch', 4: 'tex3d', 6: 'depth_test', 7: 'stencil_test'}
SAMPLER_SLOT, SAMPLER_CONSTEXPR = 1, 2


class Reader:
    def __init__(self, data: bytes):
        self.data, self.at = data, 0

    def take(self, n: int) -> bytes:
        self.at += n
        return self.data[self.at - n : self.at]

    def u32(self) -> int:
        return struct.unpack('<I', self.take(4))[0]

    def blob(self) -> bytes:
        return self.take(self.u32())

    def done(self) -> bool:
        return self.at >= len(self.data)


def program(r: Reader) -> dict:
    out = {'image': r.blob(), 'data': r.blob(), 'cbufs': list(r.blob())}
    far = r.blob()
    out['far_ubos'] = list(struct.unpack(f'<{len(far) // 2}H', far))
    words = r.u32()
    out['gprs'], out['bound_textures'] = words & 0xFF, words >> 8 & 0xFF
    return out


def words_of(blob: bytes) -> list[int]:
    return list(struct.unpack(f'<{len(blob) // 4}I', blob))


def headers(blob: bytes) -> list[list[int]]:
    return [list(struct.unpack_from('<8I', blob, 32 * i)) for i in range(len(blob) // 32)]


def load_records(path: str) -> tuple[list[dict], list[dict], dict[tuple[bool, int], bytes]]:
    """Give a records.bin's states, kernels and reflections (directpack.mm's layout).

    A reflection is a state's: a render state's under its key's hash, a compute state's under its computeStateKey's
    (a kernel key's part before its dispatch shape, as kernels.state_facts names it).
    """
    with open(path, 'rb') as f:
        r = Reader(f.read())
    states, kernel_recs, reflections = [], [], {}
    while not r.done():
        tag = r.take(4)
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
                rec['samplers'] = headers(r.blob())
                rec['spill'] = words_of(r.blob())
                rec['facts'] = struct.unpack('<3I', r.blob())
            kernel_recs.append(rec)
            continue
        if outcome == 0:
            rec['pipeline'] = r.blob()
            rec['key_string'] = r.blob().decode()
            words = struct.unpack(f'<{len(b) // 4}I', b) if (b := r.blob()) else ()
            rec['regs'] = dict(zip(words[0::2], words[1::2], strict=True))
            rec['programs'] = [program(r) for _ in range(2)]
            rec['samplers'] = headers(r.blob())
            if r.u32():
                rec['split'] = {'record': r.blob(), 'programs': [program(r) for _ in range(2)]}
            rec['variants'] = [{'assume': r.u32(), 'program': program(r)} for _ in range(r.u32())]
            rec['iterated'] = r.blob().decode()
            rec['spill'] = words_of(r.blob())
            mask, push = r.u32(), words_of(r.blob())
            rec['bound'] = {'mask': mask, 'push': push, 'spill': [words_of(r.blob()), words_of(r.blob())]}
            rec['tess_programs'] = [program(r) for _ in range(r.u32())]
            rec['extras'], rec['restore'] = (list(zip(*[iter(words_of(r.blob()))] * 2, strict=True)) for _ in range(2))
            rec['far'] = words_of(r.blob())
            rec['selectors'] = selector_section.read(r, program)
            rec['selector_generic'] = words_of(r.blob())
        states.append(rec)
    return states, kernel_recs, reflections


def stage_programs(rec: dict) -> list[dict]:
    """Give a record's vertex program and its fragment program, which a state of no fragment function has none of."""
    return [p for p in rec['programs'] if p['image']]


def program_id(program: dict) -> str:
    """Name a program by its header, code, shader data and constant buffer types (FNV-1a).

    One code can read its uniform buffers at other set-0 offsets in another state (its constant buffer types): each is
    a program of its own, so neither state binds the other's.
    """
    far = struct.pack(f'<{len(program.get("far_ubos", []))}H', *program.get('far_ubos', []))
    named = program['image'] + program['data'] + bytes(program['cbufs']) + program.get('identity', b'') + far
    return f'{packfile.fnv(named):x}'


def entries_of(
    pipeline: bytes, count: int, samplers: list[list[int]], constexpr_tsc: list[list[int]], far: bytes = b''
) -> list[dict]:
    """Give a record's set-0 entries as mkpack.py's pipeline() makes them, constexpr samplers numbered in the pack's.

    Its entries past packfile.PACK_ENTRIES are in `far`, its far block's (set_entries.h).
    """
    out = []
    for i in range(count):
        source, at = (
            (pipeline, PIPELINE.size + 8 * i) if i < packfile.PACK_ENTRIES else (far, 8 * (i - packfile.PACK_ENTRIES))
        )
        off, kind, stage, slot, sampler_kind, sampler, pad = struct.unpack_from('<HBBBBBB', source, at)
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
            if sampler_kind == SAMPLER_SLOT:
                entry['smp'] = ('slot', sampler)
                entry['compare'] = bool(pad & packfile.ENTRY_COMPARES)
            elif sampler_kind == SAMPLER_CONSTEXPR:
                header = samplers[sampler]
                if header not in constexpr_tsc:
                    constexpr_tsc.append(header)
                entry['smp'] = ('constexpr', constexpr_tsc.index(header))
            out.append(entry)
    return out


def layout_strides(key: str) -> dict[int, int]:
    """Give each Metal vertex buffer's layout stride, as the key's a<i>=format:offset:buffer:stride:... fields say."""
    out = {}
    for field in key.partition(' | ')[0].split():
        name, _, value = field.partition('=')
        parts = value.split(':')
        if name[:1] == 'a' and name[1:].isdigit() and len(parts) == 6 and all(p.isdigit() for p in parts):
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
    strides = layout_strides(key)
    for k, (buffer, bind_stride) in enumerate(buffers):
        stride = 0 if bind_stride else strides.get(buffer)
        held = regs.get(STREAM_FORMAT + 0x10 * k, 0) & STREAM_STRIDE
        if stride is None or stride > STREAM_STRIDE or held != stride:
            raise ValueError(f'vertex stream {k} of buffer {buffer} at stride {held}, its layout {stride}: {key}')


def pipeline_of(rec: dict, varying: list[int], prog_index: dict[str, int], constexpr_tsc: list[list[int]]) -> dict:
    """Give a state's pipeline as mkpack.py's pipeline() gives the probe's."""
    p = rec['pipeline']
    if len(p) != PIPELINE_BYTES:
        raise ValueError('pipeline record has an obsolete layout')
    _hash, _key, _vs, _fs, _blob, _dwords, entries, _pushes, slot, entries_high, stride, set_bytes, flags = (
        PIPELINE.unpack_from(p)
    )
    entries |= entries_high << 8
    far = rec.get('far', [])
    far_entries = max(entries - packfile.PACK_ENTRIES, 0)
    masks = (far_entries + 31) // 32
    if len(far) != 2 * far_entries + 2 * masks:
        raise ValueError(f'{rec["key"]}: a far block of {len(far)} words for {entries} entries')
    far_bits = [sum(far[2 * far_entries + m * masks + i] << 32 * i for i in range(masks)) for m in range(2)]
    mask, *words = struct.unpack_from('<33I', p, PIPELINE.size + packfile.PACK_ENTRIES * 8)
    *written_format, xr_slots = packfile.PIPELINE_V22.unpack_from(p, V22_AT)
    head, array_length_word, pulled, streams, viewport_count_word = packfile.PIPELINE_V21.unpack_from(p, V21_AT)
    stride_twin, layer_word, viewport_word = packfile.PIPELINE_V24.unpack_from(p, V24_AT)[2:5]
    streams = streams[: streams.index(0)] if 0 in streams else streams
    tail = V20_AT - packfile.PIPELINE_V18.size - packfile.PIPELINE_V15.size
    half, _reserved = struct.unpack_from('<HH', p, tail - 56)
    storage_mask, storage_write_mask, storage_samples, storage_tests, *storage_tic = struct.unpack_from(
        '<BBBB8I', p, tail - 52
    )
    depth_word, stencil_word, test_flags, shadow_word, depth_tic, stencil_tic, _reserved = struct.unpack_from(
        '<BBBBIII', p, tail - 16
    )
    v15 = packfile.PIPELINE_V15.unpack_from(p, tail)
    _variants, vertex_variants, _fragment_variants, storage_blend_mask, storage_blend_model = v15[:5]
    bound_entries, uniform, fetch_rung, _variant_name = v15[5:]
    (memory_entries,) = packfile.PIPELINE_V18.unpack_from(p, tail + packfile.PIPELINE_V15.size)
    v20 = packfile.PIPELINE_V20.unpack_from(p, V20_AT)
    heap_arrays, heap_bytes, heap_words = v20[6], v20[7], v20[8:24]
    (spill_high,) = struct.unpack_from('<H', p, V22_AT + packfile.V22_SPILL_HIGH)
    spill_entry = [v20[24] | (spill_high & 0xFF) << 8, v20[25] | (spill_high >> 8) << 8]
    spill_words, spill = v20[26:28], rec.get('spill', [])
    regs = rec['probe']['records'][0]['regs']
    check_streams(rec['key'], slot, bool(head[3]), streams, regs)
    targets = rec['probe']['targets']
    varying = list(varying)
    # a shader-side stencil's program runs in the ordered interlock as a storage form's does
    if storage_mask or shadow_word:
        for method in [0x0210, 0x0754, 0x1224] + [0x1A00 + 4 * i for i in range(8) if storage_mask >> i & 1]:
            if method not in varying:
                varying.append(method)
    if flags & 256 and 0x1224 not in varying:  # PipelineRasterOrdered: its interlock control is its own
        varying.append(0x1224)
    return {
        'index': rec['probe']['index'],
        'key': rec['key'],
        'form': rec['probe']['form'],
        'vs': prog_index[program_id(rec['programs'][0])],
        'fs': prog_index[program_id(rec['programs'][1])] if rec['programs'][1]['image'] else packfile.NO_PROGRAM,
        'blob': [(m, regs.get(m, 0)) for m in varying if m not in PROGRAM_OFFSETS and not mkpack.foreign(m, targets)]
        + rec.get('extras', []),
        'restore': rec.get('restore', []),
        'tess': [prog_index[program_id(t)] for t in rec.get('tess_programs', [])] or [0, 0],
        'runtime_head': head,
        'pulled_stride': pulled,
        'stride_twin': stride_twin,
        'amplification_layer_word': layer_word,
        'amplification_viewport_word': viewport_word,
        'push': [(0x700 + 4 * i, words[i]) for i in range(32) if mask >> i & 1],
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
        'entries': entries_of(
            p, entries, rec['samplers'], constexpr_tsc, struct.pack(f'<{2 * far_entries}I', *far[: 2 * far_entries])
        ),
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
                'program': prog_index[program_id(v['program'])],
            }
            for i, v in enumerate(rec.get('variants', []))
        ],
        'selectors': selector_section.of_record(rec, lambda p: prog_index[program_id(p)]),
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


def gather(buckets: list[str], context: dict[int, int]) -> tuple[list[dict], list[dict]]:
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
    split_identities(states)
    for index, rec in enumerate(states):
        rec['probe'] = probe_of(rec, index, context)
    return states, refused


def probe_of(rec: dict, index: int, context: dict[int, int]) -> dict:
    """Give a made state as mkpack.py takes a probe's.

    Its registers are laid over `context`, less those of the targets its draw does not write (mkpack.targets).
    """
    n = mkpack.targets(rec['key'])
    regs = dict(context)
    regs.update(rec['regs'])
    programs = stage_programs(rec)
    for method, program in zip(PROGRAM_OFFSETS, programs, strict=False):
        regs[method] = int(program_id(program), 16) & 0xFFFFFFFF
    regs = {m: 0 if mkpack.disabled_stream(m, v) else v for m, v in regs.items() if not mkpack.foreign(m, n)}
    ids = [program_id(program) for program in programs]
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


def split_identities(states: list[dict]) -> None:
    """Keep distinct splits of identical whole programs addressable, with one shared whole-program heap slot."""
    variants: dict[str, set[bytes]] = {}
    identities = []
    for rec in states:
        whole = program_id(rec['programs'][1])
        split = rec.get('split')
        identity = (
            split['record'] + b''.join(bytes.fromhex(program_id(p).zfill(16)) for p in split['programs'])
            if split
            else b''
        )
        variants.setdefault(whole, set()).add(identity)
        identities.append((whole, identity))
    for rec, (whole, identity) in zip(states, identities, strict=True):
        if len(variants[whole]) > 1:
            rec['programs'][1] = dict(rec['programs'][1], identity=identity, storage_id=whole)


def split_programs(states: list[dict], programs: dict[str, dict]) -> dict[str, dict]:
    """Give the states' splits by their whole program's id; their per-pixel programs and prologues join `programs`."""
    found = {}
    for rec in states:
        if 'split' not in rec:
            continue
        fs, prologue = rec['split']['programs']
        ids = [program_id(p) for p in (fs, prologue)]
        programs.update(zip(ids, (fs, prologue), strict=True))
        found[program_id(rec['programs'][1])] = splits.of_record(rec['split']['record'], *ids)
    return found


def lay_out_variants(states: list[dict], programs: dict[str, dict], laid: tuple, report: list[str]) -> None:
    """Append the states' variant and tessellation programs to the heap image `laid` holds, after every generic one."""
    count = tessellating = 0
    for rec in states:
        for p in (
            [v['program'] for v in rec.get('variants', [])]
            + rec.get('tess_programs', [])
            + selector_section.programs(rec)
        ):
            pid = program_id(p)
            programs.setdefault(pid, p)
            packfile.place_graphics(pid, programs, *laid)
        count += len(rec.get('variants', []))
        tessellating += bool(rec.get('tess_programs'))
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


def main() -> None:
    out_dir, *buckets = sys.argv[1:]
    selector_table = read_option(buckets, '--ca-selectors')
    inventory = read_option(buckets, '--archives') or ''
    warm = next(d for d in buckets if os.path.exists(os.path.join(d, 'context.log')))
    _, post = mkpack.load_init(os.path.join(warm, 'context.log'))
    states, refused = gather(buckets, draw_context(post[0]))
    programs = {program_id(p): p for rec in states for p in stage_programs(rec)}
    found = split_programs(states, programs)
    simple = [rec['probe'] for rec in states]
    report = [f'{len(states) + len(refused)} states made directly, {len(states)} records, {len(refused)} refused']
    if selector_table is not None:
        report += selector_section.coverage(states + refused, selector_table, packfile.key_hash)
    report += variant_report(states)
    varying, baseline = mkpack.split_registers(simple, post)
    report.append(f'{len(varying)} per-pipeline registers: ' + ' '.join(f'{m:x}' for m in varying))
    report.append(f'{len(baseline)} baseline registers over post-init')
    laid = mkpack.lay_out_programs(simple, programs, report)
    heap, prog_index, prog_recs = laid
    lay_out_variants(states, programs, laid, report)
    constexpr_tsc: list[list[int]] = []
    pipes = sorted((pipeline_of(rec, varying, prog_index, constexpr_tsc) for rec in states), key=packfile.key_hash)
    pipes, iterated = fetchfree_link.attach(pipes, report)
    computes, compute_facts = compute_kernels(buckets, constexpr_tsc, laid, report)
    used = list(dict.fromkeys(prog_recs[rec['fs']]['id'] for rec in pipes if rec['fs'] != packfile.NO_PROGRAM))
    split = splits.lay_out(used, found, programs, laid, report)
    init_recs = init_records(os.path.join(warm, 'context.log'))
    declared, kinds = archives.declared(inventory)
    held = archives.held_kernels([k['key'] for k in computes])
    report.append(archives.report(declared, kinds, held))
    reflections = {}
    for d in buckets:
        for state, blob in load_records(os.path.join(d, 'records.bin'))[2].items():
            reflections.setdefault(state, blob)
    report.append(
        f'{sum(not c for c, _ in reflections)} render and {sum(c for c, _ in reflections)} compute reflections, '
        f'{len(set(reflections.values()))} distinct'
    )
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
    )
    data, blob = packfile.pack_bytes(pack)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'pack.bin'), 'wb') as f:
        f.write(data)
    packfile.write_json(os.path.join(out_dir, 'pack.json'), pack, varying)
    packfile.write_layouts(os.path.join(out_dir, 'layouts.txt'), pack)
    write_programs(os.path.join(out_dir, 'programs.bin'), programs)
    unshared = sum(len(p['blob']) * 2 for p in pipes)
    report.append(
        f'pack.bin {len(data) // 1024} KiB: {len(pipes)} pipelines, {len(computes)} kernels, '
        f'{len(prog_recs)} programs, {len(split)} splits, {len(constexpr_tsc)} constexpr samplers, '
        f'{len(blob)} blob dwords ({unshared} unshared), {len(baseline)} baseline regs'
    )
    report += [f'  refused ({rec["why"]}): {rec["key"][:150]}' for rec in refused]
    with open(os.path.join(out_dir, 'report.txt'), 'w') as f:
        f.write('\n'.join(report) + '\n')
    print('\n'.join(line for line in report if not line.startswith('  refused')))


if __name__ == '__main__':
    main()
