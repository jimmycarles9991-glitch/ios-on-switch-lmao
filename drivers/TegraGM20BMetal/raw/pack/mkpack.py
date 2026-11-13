#!/usr/bin/env python3
"""mkpack (r7-raw): the GM20B pipeline pack for the raw Metal path, read off the probe run.

The probe run: capture/probe.mm through the live frontend + m2v + NVK twin with nvdec in NVDEC_PROBE + NVDEC_SCRUB +
NVDEC_TRACE=3 mode.

For every corpus render pipeline state the raw path can take (one draw, no m2v-internal passes or dispatches):
  * its two programs: the 80-byte shader header + Maxwell code NAK made (from the NVK driver records), laid out in one
    shader heap image the way NVK uploads them pre-Turing (header at +0x30, code at +0x80 of a 0x100-aligned slot);
  * the 3D registers it sets (the registers that differ between states), as (method, value) pairs;
  * its root-table push words and its set-0 layout: which byte of the 256-byte descriptor set holds which Metal buffer
    slot's uniform descriptor, texture/sampler handle or framebuffer-fetch handle;
  * which Metal vertex buffer slot feeds vertex stream 0.
Plus what every state shares: the context init NVK records at queue creation (with the addresses to relocate marked) and
the registers every state's draws leave the same (the baseline).

usage: mkpack.py <merged programs.bin out> <out dir> <bucket dir>...
  each bucket dir (rawpack.py's) holds its own probe runs: probe256.out/.log, and the PROBE_BYTES=16 and 4096 runs
  (probe16.*, probe4096.*: each buffer entry's descriptor type, a uniform buffer's range NVK rounds up to 256 bytes, and
  the function's window on the slot) and programs.bin; a state's index is (its bucket, its line in the bucket's corpus).
  The context init and post-init registers are the first bucket's (every probe process opens the device alike). A
  bucket's barrier/ holds its storage-form states' run with NVK's interlock hidden (rawpack.py barrier_set): their
  record is the barrier form (PipelineBarrierForm) from it.
writes the merged programs.bin, <out>/pack.bin (runtime/raw_pack.h layout), <out>/pack.json, <out>/programs.txt
(decoder tables), and prints a report.
"""

import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import layouts
import packfile
import probelog


def owned_ranges() -> tuple[list[tuple[int, int]], tuple[int, int]]:
    """Give the methods the raw path owns (runtime/raw_owned.h kOwnedMethods) and the vertex streams' range."""
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'runtime', 'raw_owned.h')) as header:
        text = header.read()
    pair = r'\{(0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)\}'
    table = text[text.index('kOwnedMethods[]') : text.index('};', text.index('kOwnedMethods[]'))]
    streams = re.search(r'kVertexStreams = ' + pair, text)
    return [(int(a, 16), int(b, 16)) for a, b in re.findall(pair, table)], (int(streams[1], 16), int(streams[2], 16))


# Registers the runtime owns (sets per pass, per draw, or once at init with its own addresses): never in a blob or the
# baseline.
OWNED, VERTEX_STREAMS = owned_ranges()


def owned(m: int) -> bool:
    if VERTEX_STREAMS[0] <= m < VERTEX_STREAMS[1] and (m & 0xF) in (4, 8):  # vertex stream locations
        return True
    return any(a <= m < b for a, b in OWNED)


# Init relocations: (subc, method) -> what the runtime puts there
RELOC = {
    (0, 0x1608): 'heap_hi',
    (0, 0x160C): 'heap_lo',
    (1, 0x1608): 'heap_hi',
    (1, 0x160C): 'heap_lo',
    (0, 0x0F84): 'zero_hi',
    (0, 0x0F88): 'zero_lo',
    (0, 0x3468): 'root_hi',
    (0, 0x346C): 'root_lo',
    (0, 0x3470): 'zero_hi',
    (0, 0x3474): 'zero_lo',
    (0, 0x1574): 'tic_hi',
    (0, 0x1578): 'tic_lo',
    (0, 0x157C): 'tic_max',
    (1, 0x1574): 'tic_hi',
    (1, 0x1578): 'tic_lo',
    (1, 0x157C): 'tic_max',
    (0, 0x155C): 'tsc_hi',
    (0, 0x1560): 'tsc_lo',
    (0, 0x1564): 'tsc_max',
    (1, 0x155C): 'tsc_hi',
    (1, 0x1560): 'tsc_lo',
    (1, 0x1564): 'tsc_max',
}
RELOC_CODE = {
    'heap_hi': 1,
    'heap_lo': 2,
    'zero_hi': 3,
    'zero_lo': 4,
    'root_hi': 5,
    'root_lo': 6,
    'tic_hi': 7,
    'tic_lo': 8,
    'tic_max': 9,
    'tsc_hi': 10,
    'tsc_lo': 11,
    'tsc_max': 12,
}

# The root table's interlock ticket buffer (nvk_root_descriptor_table.interlock_ticket_buffer): the two words NVK's init
# loads after LOAD_CONSTANT_BUFFER_OFFSET 0x260, PackReloc 13 and 14 (the kit's context.tsv tickets_lo and tickets_hi)
RELOC_TICKETS = (13, 14)


def init_relocations(init: list[tuple[int, int, int]]) -> list[tuple[int, int, int, int]]:
    """Give the init's methods with their PackReloc codes: RELOC's by method, the ticket words by their place."""
    out, ticket = [], None
    for sc, m, d in init:
        code = RELOC_CODE.get(RELOC.get((sc, m)), 0)
        if sc == 0 and m == 0x238C:
            ticket = 0 if d == 0x260 else None
        elif sc == 0 and m == 0x2390 and ticket is not None and ticket < len(RELOC_TICKETS):
            code = RELOC_TICKETS[ticket]
            ticket += 1
        out.append((sc, m, d, code))
    return out


def load_programs(path: str) -> dict[str, dict[str, bytes | list[int]]]:
    """Give mkprograms.py's programs by the id nvdec names a bound one with: the FNV-1a of its header, code and data."""
    with open(path, 'rb') as programs:
        b = programs.read()
    progs, at = {}, 0
    while at + 16 <= len(b):
        assert b[at : at + 4] == b'NVPG'
        code, count, data = struct.unpack_from('<III', b, at + 4)
        at += 16
        types = list(b[at : at + count])
        at += count
        image = b[at : at + 80 + code]
        at += 80 + code
        progs[f'{packfile.fnv(b[at - 80 - code : at + data]):x}'] = {
            'image': image,
            'cbufs': types,
            'data': b[at : at + data],
        }
        at += data
    return progs


def load_buckets(dirs: list[str], size: int) -> list[dict]:
    """Give every bucket's probes of the run with `size`-byte payloads, each with its bucket's number and form.

    The form is 'barrier' in a barrier bucket (the run with NVK's interlock hidden, rawpack.py), else ''.
    """
    probes = []
    for b, d in enumerate(dirs):
        form = 'barrier' if os.path.basename(os.path.normpath(d)).startswith('barrier') else ''
        for p in probelog.load(os.path.join(d, f'probe{size}.out'), os.path.join(d, f'probe{size}.log')):
            p['bucket'] = b
            p['form'] = form
            probes.append(p)
    return probes


def interlocked(p: dict) -> bool:
    """Whether a probe draw had SET_PIXEL_SHADER_INTERLOCK_CONTROL on: metal2vulkan's storage form of a fetch.

    A fetched color whose texture lacks STORAGE is drawn in a scratch that has it, between a copy in and a copy out
    (full-screen draws of m2v's own): three draws, the state's the middle one.
    """
    return any(r and r.get('regs', {}).get(0x1224, 0) & 3 for r in p['records'])


def storage_keys(probes: list[dict]) -> set[str]:
    """Give the keys of the states the default runs drew in the storage form, in one draw or through a scratch."""
    return {p['key'] for p in probes if not p['form'] and 'failed' not in p and interlocked(p)}


def default_forms(simple: list[dict], storage: set[str]) -> tuple[list[dict], list[tuple[dict, str]]]:
    """Give each state its default record and set the rest aside with why.

    A state metal2vulkan draws in the storage form (the pixel interlock) gets its barrier form (input attachments,
    ordered by barriers: the barrier run's) as its record; layouts.py cannot name the storage form's image, so it is not
    packed.
    """
    barrier = {p['key'] for p in simple if p['form']}
    kept, aside = [], []
    for p in simple:
        if not p['form'] and p['key'] in storage:
            aside.append((p, 'storage form' + ('' if p['key'] in barrier else ', its barrier form not taken')))
        elif p['form'] and p['key'] not in storage:
            # a barrier run's first line is its bucket's warm-up state (rawpack.py barrier_set), probed in both runs
            aside.append(
                (p, 'warm-up state' if p['index'] == 0 else 'barrier form of a state drawn without the interlock')
            )
        else:
            kept.append(p)
    return kept, aside


# NVK's cbuf types (nvk_shader.h nvk_cbuf_type) a raw draw binds: the root table at slot 0, then NAK's shader data,
# set 0, and set 0's uniform buffers (mkprograms.py cbuf_code: the type in bits 0-2, the descriptor's offset / 16 above)
# at the slots the program names (raw_gpu.cpp)
CBUF_ROOT, CBUF_SHADER_DATA, CBUF_SET, CBUF_UBO = 1, 2, 3, 5
CBUF_HEAP_SET = CBUF_SET | 1 << 3  # runtime/raw_pack.h kCbufHeapSet: set 1, a heap's arrays


def ubo_offsets(program: dict) -> list[int]:
    """Return the set-0 byte offsets of the uniform buffers the program reads as constant buffers."""
    return [(c >> 3) * 16 for c in program['cbufs'][1:] if c & 7 == CBUF_UBO]


def raw_bindable(program: dict, heap: bool = False) -> bool:
    """Give whether a raw draw binds every constant buffer `program` reads; set 1 only with a heap (`heap`)."""
    cbufs = program['cbufs']
    rest = [c for c in cbufs[1:] if c & 7 != CBUF_UBO and not (heap and c == CBUF_HEAP_SET)]
    offsets = ubo_offsets(program)
    return (
        bool(cbufs)
        and cbufs[0] == CBUF_ROOT
        and set(rest) <= {CBUF_SHADER_DATA, CBUF_SET}
        and len(set(rest)) == len(rest)
        and len(set(offsets)) == len(offsets)
        and (CBUF_SHADER_DATA in rest) == bool(program['data'])
    )


def raw_bound(simple: list[dict], programs: dict[str, dict]) -> tuple[list[dict], list[tuple[dict, str]]]:
    """Set aside the states whose programs read constant buffers a raw draw does not bind."""
    kept, aside = [], []
    for p in simple:
        unbound = [
            programs[pid]['cbufs']
            for pid in (p['records'][0]['prog'][st][0] for st in (1, 5))
            if not raw_bindable(programs[pid])
        ]
        if unbound:
            aside.append(
                (
                    p,
                    (
                        f'a program reads constant buffers {unbound[0]}, the raw path binds root, shader data, set 0 '
                        'and its uniform buffers'
                    ),
                )
            )
        else:
            kept.append(p)
    return kept, aside


# Why a probed state may stay out of the pack (the raw path hands it to m2v): what its draw needs that the raw path
# draws otherwise. Any other reason is a generator fault and fails the build, m2v failing a state's draw among them: its
# pipeline did not build (a compiler regression: 39 RenderBox states once failed in NAK's encoder), and the device would
# draw nothing for it either.
EXPECTED_SKIPS = (
    'draws',
    'dispatches',
    'storage form',
    'warm-up state',
    'a sampler m2v derives from a bound one',
    'the raw path binds root, shader data, set 0 and its uniform buffers',
    'as a uniform buffer the layout does not name',
    'set-0 entries',
    "a buffer of m2v's own",
    'no fragment stage',
)
# raw_pack.h kNoSlot: a texture3d argument's Metal index, which the probe cannot tell (m2v binds its own 3D view
# there); the direct compile's records name it
NO_SLOT = 0xFF


def unexpected(why: str) -> bool:
    return not any(reason in why for reason in EXPECTED_SKIPS)


def account(report: list[str], skipped: list[tuple[dict, str]], taken: int, packed: int) -> list[tuple[dict, str]]:
    """List every record set aside with its reason, count them by reason, and give those set aside by a fault."""
    faults = [(p, why) for p, why in skipped if unexpected(why)]
    for p, why in skipped:
        report.append(f'  {"FAULT" if unexpected(why) else "skipped"} {p["index"]} ({why}): {p["key"][:150]}')
    expected = collections.Counter(
        next(reason for reason in EXPECTED_SKIPS if reason in why) for _, why in skipped if not unexpected(why)
    )
    report.append('set aside for m2v: ' + '; '.join(f'{n} x {reason}' for reason, n in expected.most_common()))
    report.append(f'{taken} taken, {packed} packed, {len(faults)} dropped by a generator fault')
    return faults


def load_sizes(dirs: list[str]) -> dict[tuple, list[int]]:
    sizes = {}  # (bucket, state index, set offset) -> (bound size with 16-byte payloads, with 4096-byte payloads)
    for b in (16, 4096):
        for p in load_buckets(dirs, b):
            if p['draws'] != 1 or p['dispatches'] or not p['records'] or 'set0' not in p['records'][0]:
                continue
            w = p['records'][0]['set0'][1]
            for o in range(0, len(w) - 3, 4):
                if f'ptr0.{o * 4}' in p['records'][0]['ptr']:
                    sizes.setdefault((p['bucket'], p['index'], o * 4), [0, 0])[0 if b == 16 else 1] = w[o + 2]
    return sizes


# The copy engine's subchannel: NVK's init uses it only to fill buffers of its own (the pixel interlock's ticket buffer,
# zeroed at the probe's address), which are not context state; replayed, the fill would write the probe's address
SUBC_COPY = 4


def load_init(log_path: str) -> tuple[list[tuple[int, int, int]], dict[int, dict[int, int]]]:
    # init trace and post-init registers
    init, post = [], {0: {}, 1: {}}
    with open(log_path) as log:
        for line in log:
            if line.startswith('T '):
                _, sc, m, d = line.split()
                if int(sc) != SUBC_COPY:
                    init.append((int(sc), int(m, 16), int(d, 16)))
            elif line.startswith('S regs '):
                f = line.split()
                post[int(f[2])] = {int(a, 16): int(v, 16) for a, v in (x.split('=') for x in f[3:])}
    return init, post


def record_reason(records: list[dict]) -> str | None:
    if not records:
        return 'no record'
    r = records[0]
    if any(v[2] != 'known' for v in r['prog'].values()):
        return 'unknown program'
    if sorted(r['prog']) == [1]:
        return 'no fragment stage'
    if sorted(r['prog']) != [1, 5]:
        return f'stages {sorted(r["prog"])}'
    return None


def skip_reason(p: dict) -> str | None:
    if 'failed' in p:
        return f'm2v failed its draw: {p["failed"]}'
    if p['draws'] != 1 and not p['dispatches'] and not p['form'] and interlocked(p):
        return 'storage form through a scratch, m2v copying the color in and out around it'
    if p['draws'] != 1:
        return f'{p["draws"]} draws'
    if p['dispatches']:
        return f'{p["dispatches"]} dispatches'
    return record_reason(p['records'])


def split_probes(probes: list[dict]) -> tuple[list[dict], list[tuple[dict, str]]]:
    simple, skipped = [], []
    for p in probes:
        why = skip_reason(p)
        # (three color attachments: the render server's since r7n's harvest, Core Animation's "A3" states with an
        # R32Float third attachment, taken as any other; test/frame.h FRAME_A3 draws them)
        if why:
            skipped.append((p, why))
        else:
            simple.append(p)
    return simple, skipped


# a color target's own registers (per-target blend 0x1e00 + 0x20 j, SET_CT_WRITE 0x1a00 + 4 j, SET_BLEND 0x1360 + 4 j)
# mean nothing for a state without target j: what the probe saw there is an earlier state's leftover. Not that state's,
# so neither in its blob nor in deciding what varies (r8-rawca: two states with a third target made targets 2's blend
# "vary" for every other state)
def targets(key: str) -> int:
    """Give the color attachments a key names, bit j its c<j>."""
    return sum(1 << int(t[1]) for t in key.split() if t[:1] == 'c' and t[1:2].isdigit() and '=' in t)


XR_PLANES = 550  # RGB10A8_2P_XR10 (runtime/raw_xr.h XrForm::Planes)


def colour_formats(key: str) -> list[int]:
    """Give the MTLPixelFormat of each colour a key names, by index, 0 for none."""
    formats = [0] * 8
    for t in key.partition(' | ')[0].split():
        if t[:1] == 'c' and t[1:2].isdigit() and t[2:3] == '=':
            formats[int(t[1])] = int(t[3:].partition(':')[0])
    return formats


def xr_alpha_slots(formats: list[int]) -> list[int]:
    """Give each colour's alpha index as runtime/raw_xr_pass.h xrAlphaSlots does, -1 for none.

    A two-plane RGB10A8_2P_XR10 colour's alpha byte is a target of its own: the lowest index no colour holds and no
    earlier colour's alpha took. mkpack_owned_test holds this to raw_xr_pass_test's cases.
    """
    alpha, free = [-1] * 8, 0
    for j, f in enumerate(formats):
        if f != XR_PLANES:
            continue
        while free < 8 and formats[free]:
            free += 1
        if free == 8:
            break
        alpha[j] = free
        free += 1
    return alpha


def target_mask(key: str, two_plane: bool) -> int:
    """Give the targets a state's draw writes, bit j target j.

    Those are its key's colours and, in its two-plane form (PipelineTwoPlane), each RGB10A8_2P_XR10 colour's alpha
    target.
    """
    alpha = xr_alpha_slots(colour_formats(key)) if two_plane else []
    return targets(key) | sum(1 << a for a in alpha if a >= 0)


def foreign(m: int, mask: int) -> bool:
    """Whether `m` is a color target's own register for a target `mask` lacks, past its last or between two."""
    for base, stride in ((0x1E00, 0x20), (0x1A00, 4), (0x1360, 4)):
        if base <= m < base + 8 * stride and not mask >> ((m - base) // stride) & 1:
            return True
    return False


# SET_VERTEX_STREAM_A_FORMAT(j) with ENABLE (bit 12) clear: a stream the hardware does not fetch, whose stride the probe
# saw as an earlier state's leftover. Taken as 0, which the direct compile (runtime/raw_emit.cpp) makes too
def disabled_stream(m: int, v: int) -> bool:
    return 0x1C00 <= m < 0x1E00 and (m - 0x1C00) % 0x10 == 0 and not v & 0x1000


def split_registers(simple: list[dict], post: dict[int, dict[int, int]]) -> tuple[list[int], dict[int, int]]:
    # registers: varying across states -> per-pipeline blob; equal -> baseline (when it differs from post-init). A
    # register foreign to a state (a target it lacks) is not its, so it decides nothing: a state with no color target
    # (Portrait's depth-only states) would otherwise move target 0's per-target blend (0x1e00) out of the baseline, and
    # every record would take it from the warm-up draw where the runtime's direct compile takes the init template's.
    # A state's targets are its maker's mask (p['targets']): a two-plane state's alpha targets are its own too
    allregs = set(post[0])
    for p in simple:
        allregs |= set(p['records'][0]['regs'])
    vals = {
        m: {p['records'][0]['regs'].get(m, 0) for p in simple if not foreign(m, p['targets'])}
        for m in allregs
        if not owned(m)
    }
    varying = sorted(m for m, v in vals.items() if len(v) > 1)
    baseline = {m: next(iter(v)) for m, v in vals.items() if len(v) == 1 and next(iter(v)) != post[0].get(m, 0)}
    return varying, baseline


def lay_out_programs(
    simple: list[dict], programs: dict[str, dict], report: list[str]
) -> tuple[bytearray, dict[str, int], list[dict]]:
    # programs used, heap layout
    used = []
    for p in simple:
        for st in (1, 5):
            pid = p['records'][0]['prog'][st][0]
            if pid not in used:
                used.append(pid)
    heap, prog_index, prog_recs = bytearray(), {}, []
    for pid in used:
        packfile.place_graphics(pid, programs, heap, prog_index, prog_recs)
    data = sum(1 for pr in prog_recs if pr['data_bytes'])
    report.append(f'{len(prog_recs)} programs ({data} with shader data), shader heap image {len(heap) // 1024} KiB')
    return heap, prog_index, prog_recs


# SET_PIPELINE_REGISTER_COUNT(vertex B, pixel): the probe's register count of each program it bound
REGISTER_COUNTS = {1: 0x204C, 5: 0x214C}


def probed_register_counts(simple: list[dict], programs: dict[str, dict]) -> dict[str, dict]:
    """Give each program the register count its states' draws set (PackProgram.gprs: owned, so in no blob)."""
    for p in simple:
        record = p['records'][0]
        for stage, method in REGISTER_COUNTS.items():
            programs[record['prog'][stage][0]]['gprs'] = record['regs'].get(method, 0)
    return programs


FRAGMENT_CODES = (1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15)


def bind_offset(stage: str, slot: int) -> int:
    """Give the offset the probe's bind-offset run binds a slot's payload at (probe_bind.h probeBindOffset)."""
    if stage == 'f':
        return 16 * (12 + slot // 12) + FRAGMENT_CODES[slot % 12]
    return 16 * (1 + slot // 3) + 4 * (1 + slot % 3)


def storage_code(stage: str, slot: int) -> int:
    """Give the residual a storage slot's push word holds in the bind-offset run (probe_bind.h probeStorageCode)."""
    return bind_offset(stage, slot) & 15


def push_words(p: dict) -> dict[int, int]:
    return {int(o, 16): int(x, 16) for o, x in (w.split('=') for w in p['records'][0].get('pushw', []))}


def attach_residuals(dirs: list[str], probes: list[dict]) -> None:
    """Give each probe ('residuals') the root push words the bind-offset run changed, by their value's low byte.

    A slot's SlotBindOffset residual is its bind offset less the multiple of m2v's base alignment (NVK's 256 bytes for a
    uniform buffer, 16 for a storage one) at or below where its window starts: in that run, the slot's own offset
    modulo the alignment (a window starting past it makes it negative), 0 in the 256-byte run's. A low byte two words
    share names neither (runtime/raw_residual.h residualWordsByValue). A bucket without the run gives none (such states
    take only aligned offsets).
    """
    residuals = {}
    for b, d in enumerate(dirs):
        out, log = os.path.join(d, 'probeoff.out'), os.path.join(d, 'probeoff.log')
        if not os.path.exists(out):
            continue
        plain = {
            p['index']: push_words(p)
            for p in probelog.load(os.path.join(d, 'probe256.out'), os.path.join(d, 'probe256.log'))
            if p['records']
        }
        for p in probelog.load(out, log):
            if p['draws'] != 1 or p['dispatches'] or not p['records'] or p['index'] not in plain:
                continue
            was = plain[p['index']]
            changed = [(v & 0xFF, o) for o, v in push_words(p).items() if v and not was.get(o)]
            values = collections.Counter(v for v, _ in changed)
            residuals[(b, p['index'])] = {v: o for v, o in changed if values[v] == 1}
    for p in probes:
        p['residuals'] = residuals.get((p['bucket'], p['index']), {})


def is_storage(e: tuple, index: tuple[int, int], sizes: dict[tuple, list[int]]) -> bool:
    """Tell a storage buffer: its 16-byte payload bound at 256 keeps 16 bytes (NVK rounds a uniform one's to 256)."""
    return sizes.get((*index, e[0]), (256, 256))[0] < 256


def buffer_entry(
    e: tuple, index: tuple[int, int], sizes: dict[tuple, list[int]], words: dict[int, int], shared: set[int]
) -> dict:
    """Give a buffer entry; a storage one's residual word where no other storage slot of the state shares its code."""
    s4k = sizes.get((*index, e[0]), (256, 256))[1]
    # the 4096-byte payload's range: the window (0: the whole payload)
    ssbo = is_storage(e, index, sizes)
    window = 0 if s4k >= 4096 else s4k
    code = storage_code(e[2], e[3])
    return {
        'off': e[0],
        'kind': 'ubo',
        'stage': e[2],
        'slot': e[3],
        'size': e[4],
        'ssbo': ssbo,
        'window': window,
        'residual': (None if code in shared else words.get(code)) if ssbo else words.get(bind_offset(e[2], e[3])),
    }


def handle_entry(e: tuple, tsc: dict[int, list[int]], constexpr_tsc: list[list[int]], problems: list[str]) -> dict:
    tk, sk = e[2], e[3]
    ent = {'off': e[0], 'kind': 'handle'}
    if tk is None:
        ent['tex'] = None
    elif tk[0] == 'tex':
        ent['tex'] = ('slot', tk[1])
    elif tk[0] == 'fetch':
        ent['tex'] = ('fetch', tk[1])
    elif tk[0] == 'tex3d':
        ent['tex'] = ('tex3d', NO_SLOT)
    else:
        problems.append(f'texture {tk}')
    if sk is None:
        ent['smp'] = None
    elif sk[0] == 'smp':
        ent['smp'] = ('slot', sk[1])
        # m2v binds a slot the state samples by comparison with its compare header (TEXSAMP0 DEPTH_COMPARE, bit 9)
        ent['compare'] = bool(tsc[e[4] >> 20][0] >> 9 & 1)
    elif sk[0] == 'tsc?derived':
        problems.append('a sampler m2v derives from a bound one (YCbCr conversion)')
    else:
        # a sampler the probe did not bind: the function's constexpr sampler (made by m2v as an immutable one)
        words = tsc[e[4] >> 20]
        if words not in constexpr_tsc:
            constexpr_tsc.append(words)
        ent['smp'] = ('constexpr', constexpr_tsc.index(words))
    return ent


def vertex_streams(r: dict, problems: list[str]) -> list[dict]:
    vbs = []
    for j, (_loc, _lim, fmt) in sorted(r['vbinfo'].items()):
        w = r['vb'][j][1]
        if (w[0] >> 16) != 0xC0DE or (w[0] >> 8) & 0xFF != 1:
            problems.append(f'vertex stream {j} not a vertex slot')
        vbs.append({'stream': j, 'slot': w[0] & 0xFF, 'stride': fmt & 0xFFF})
    return vbs


def pipeline(
    p: dict,
    varying: list[int],
    prog_index: dict[str, int],
    sizes: dict[tuple[int, int], list[int]],
    constexpr_tsc: list[list[int]],
) -> dict:
    r = p['records'][0]
    ents, problems = layouts.layout(p)
    rec = {
        'index': p['index'],
        'key': p['key'],
        'form': p['form'],
        'vs': prog_index[r['prog'][1][0]],
        'fs': prog_index[r['prog'][5][0]],
    }
    rec['blob'] = [
        (m, r['regs'].get(m, 0)) for m in varying if m not in (0x2044, 0x2144) and not foreign(m, p['targets'])
    ]
    root = r['root'][1]
    rec['push'] = [(int(o, 16), int(x, 16)) for o, x in (w.split('=') for w in r.get('pushw', []))]
    rec['set_size'] = root[0x38 // 4 + 2] if 'set0' in r else 0
    index = (p['bucket'], p['index'])
    codes = collections.Counter(storage_code(e[2], e[3]) for e in ents if e[1] == 'ubo' and is_storage(e, index, sizes))
    shared = {c for c, n in codes.items() if n > 1}
    entries = [
        buffer_entry(e, index, sizes, p.get('residuals', {}), shared)
        if e[1] == 'ubo'
        else {'off': e[0], 'kind': 'color_table'}
        if e[1] == 'color_table'
        else handle_entry(e, r['tsc'], constexpr_tsc, problems)
        for e in ents
    ]
    rec['entries'] = entries
    if len(entries) > packfile.PACK_ENTRIES:
        problems.append(f'more than {packfile.PACK_ENTRIES} set-0 entries')
    rec['vbs'] = vertex_streams(r, problems)
    rec['fetch'] = any(e.get('tex', [None])[0] == 'fetch' for e in entries if e['kind'] == 'handle')
    rec['clears'] = len(p['clears'])
    rec['problems'] = problems
    return rec


def with_unnamed_ubos(rec: dict, p: dict, programs: dict[str, dict]) -> dict:
    """Return the record with a problem for each set-0 uniform buffer a stage reads that its layout does not name."""
    r = p['records'][0]
    uniform = {e['off'] for e in rec['entries'] if e['kind'] == 'ubo' and not e['ssbo']}
    rec['problems'] += [
        f'stage {stage} reads set 0 at {offset} as a uniform buffer the layout does not name'
        for stage, pid in ((1, r['prog'][1][0]), (5, r['prog'][5][0]))
        for offset in ubo_offsets(programs[pid])
        if offset not in uniform
    ]
    return rec


def main() -> None:
    prog_path, out_dir = sys.argv[1], sys.argv[2]
    dirs = [x for d in sys.argv[3:] for x in (d, os.path.join(d, 'barrier')) if os.path.isdir(x)]
    with open(prog_path, 'wb') as merged:
        for d in dirs:
            with open(os.path.join(d, 'programs.bin'), 'rb') as part:
                merged.write(part.read())
    sizes = load_sizes(dirs)
    os.makedirs(out_dir, exist_ok=True)
    seen: set[tuple[str, str]] = set()
    probes = [
        p for p in load_buckets(dirs, 256) if (p['form'], p['key']) not in seen and not seen.add((p['form'], p['key']))
    ]
    attach_residuals(dirs, probes)
    programs = load_programs(prog_path)
    init, post = load_init(os.path.join(dirs[0], 'probe256.log'))
    # drop the query-pool setup (submit 4 is not traced) and NVK's own MME bookkeeping we relocate
    report = []
    simple, skipped = split_probes(probes)
    simple, aside = default_forms(simple, storage_keys(probes))
    skipped += aside
    simple, aside = raw_bound(simple, programs)
    skipped += aside
    for p in simple:
        n = p['targets'] = targets(p['key'])
        p['records'][0]['regs'] = {
            m: 0 if disabled_stream(m, v) else v for m, v in p['records'][0]['regs'].items() if not foreign(m, n)
        }
    varying, baseline = split_registers(simple, post)
    report.append(
        f'{len(probes)} records probed, {len(simple)} taken ({sum(1 for p in simple if p["form"])} of them barrier '
        f'forms of storage-form states), {len(skipped)} set aside'
    )
    names = ' '.join(f'{m:x}' for m in varying)
    report.append(f'{len(varying)} per-pipeline registers: {names}')
    report.append(f'{len(baseline)} baseline registers over post-init')
    heap, prog_index, prog_recs = lay_out_programs(simple, probed_register_counts(simple, programs), report)
    # pipelines
    pipes, constexpr_tsc = [], []
    for p in simple:
        rec = with_unnamed_ubos(pipeline(p, varying, prog_index, sizes, constexpr_tsc), p, programs)
        if rec['problems']:  # m2v draws it (RawMetal.mm: a state not in the pack goes there)
            skipped.append((p, '; '.join(rec['problems'])))
            continue
        pipes.append(rec)
    # the runtime binary-searches the pipelines in place (raw_pack.cpp Pack::find): no process keeps an index of its own
    pipes.sort(key=packfile.key_hash)
    computes: list[dict] = []
    # init template with relocations (submissions 1-3 as NVK recorded them)
    init_recs = init_relocations(init)
    report.append(f'init template: {len(init_recs)} methods, {sum(1 for x in init_recs if x[3])} relocated')
    pack = packfile.Pack(pipes, prog_recs, constexpr_tsc, baseline, init_recs, heap, computes)
    data, blob = packfile.pack_bytes(pack)
    with open(os.path.join(out_dir, 'pack.bin'), 'wb') as pack_bin:
        pack_bin.write(data)
    packfile.write_json(os.path.join(out_dir, 'pack.json'), pack, varying)
    packfile.write_layouts(os.path.join(out_dir, 'layouts.txt'), pack)
    report.append(
        f'pack.bin {len(data) // 1024} KiB: {len(pipes)} pipelines, {len(computes)} kernels, '
        f'{len(prog_recs)} programs, '
        f'{len(constexpr_tsc)} constexpr samplers, {len(blob)} blob dwords, {len(baseline)} baseline regs'
    )
    faults = account(report, skipped, len(simple), len(pipes))
    with open(os.path.join(out_dir, 'report.txt'), 'w') as report_txt:
        report_txt.write('\n'.join(report) + '\n')
    print('\n'.join(report))
    if faults:
        reasons = collections.Counter(why.split(' (')[0][:80] for _, why in faults)
        sys.exit(
            f'mkpack: {len(faults)} records dropped for reasons no state may have (FAULT lines): '
            + '; '.join(f'{n} x {why}' for why, n in reasons.most_common(8))
        )


if __name__ == '__main__':
    main()
