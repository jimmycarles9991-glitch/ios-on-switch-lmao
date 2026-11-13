"""Classify how raw's frame differs from NVK's into the classes ruled raw's own (ORACLE.md, eq).

A draw that differs in any other way is refused, and no golden is written over it:

  baseline   the per-target blend of a colour the draw does not write, as the pack's baseline leaves it once a context:
             separate alpha on, ADD, ZERO coefficients (NVK leaves it 0)
  storage    a fetching state in raw's storage form: the interlock, colour writes through the program, its blend in the
             program, and no pixel barrier (the interlock orders its fetches)
  blendkill  SET_ITERATED_BLEND_OPTIMIZATION's zero-source kill (raw_blend_kill.h)
  shader     a fetch after storage writes, ordered by raw's shader barrier (INVALIDATE_SHADER_CACHES, a WFI) for NVK's
             PSB
  elided     a fetch raw draws without a barrier because its scissor meets no pixel written since the last one
             (raw_region.h): the draw's box and those writes' boxes, which must not overlap
  inherit    a fetch with no barrier of its own where no draw since raw's last barrier (named) writes a colour
  passstart  a pass's first draw, without the scheduler and header-cache invalidations NVK's pass start adds
  push       push words a program of only one driver reads (raw's direct-compiled programs read their own)
  fetchfree  the state's fetch-free form (raw_fetch_free.h): raw blends a colour on the ROP that NVK's program fetches
             and raw's does not, its blend registers raw's, and no pixel barrier (the ROP orders its own reads)
  noread     a draw whose programs read none of the colours NVK's fetches, without NVK's pixel barrier
             (raw_blend_reads.h: a blend whose result does not depend on the destination does not read it)
  unread     uniform contents NVK's program reads that raw's draw does not reach, where raw binds none of them and keeps
             the set-0 upload of the draw before: raw uploads set 0 again only where an entry its programs read from it
             moved (raw_descriptors.h), so none of its programs reads them (unread_uniforms_test.py: a storage form's
             in-program blend that drops the colour they feed)

Barriers only raw has are its own to add.
"""

import eq_fields

# SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA, then COLOR_OP .. ALPHA_DEST_COEFF, as the pack's baseline leaves them
BASELINE = {0: '1', 4: '8006', 8: '4000', 0xC: '4000', 0x10: '8006', 0x14: '4000', 0x18: '4000'}
STORAGE = {'1224', 'f90'} | {f'{0x1A00 + 4 * j:x}' for j in range(8)} | {f'{0x1360 + 4 * j:x}' for j in range(8)}
PUSH_WORDS = {17, 18}  # read by NVK's fixed_frag programs only (ORACLE.md: c[0x0][0x744])
SHADER_BARRIER = {'SHINV', 'WFI'}
PIXEL = {'PSB', 'TEXINV'}
PASS_START = {'SKEDINV', 'TICINV', 'TSCINV', 'CE_COPY'}


def per_target_blend(m: str) -> bool:
    return 0x1E00 <= int(m, 16) < 0x1F00


def baseline(m: str, value: str, draw: eq_fields.Draw) -> bool:
    """Say whether m is a per-target blend register at the baseline's value, of a colour the draw does not write."""
    if not per_target_blend(m):
        return False
    target, at = divmod(int(m, 16) - 0x1E00, 0x20)
    return BASELINE.get(at) == value and draw.registers.get(f'{0x1A00 + 4 * target:x}', '0') == '0'


def blend_target(m: str) -> int | None:
    """Give the colour m is a blend register of: SET_BLEND(j) or the per-target blend block j."""
    v = int(m, 16)
    if 0x1360 <= v < 0x1380:
        return (v - 0x1360) // 4
    return (v - 0x1E00) // 0x20 if per_target_blend(m) else None


def reads_target(draw: eq_fields.Draw, j: int) -> bool:
    """Say whether a stage of the draw reaches colour target j's texture header: its program fetches the colour."""
    target = draw.fields.get(f'rt{j}')
    name = target.split()[0].split('=', 1)[1] if target else None
    return bool(name) and any(f'tic[{name} ' in c for s in draw.stages.values() for c in s.content)


def fetched(draw: eq_fields.Draw) -> set[int]:
    return {j for j in range(8) if reads_target(draw, j)}


def fetch_free(nvk: eq_fields.Draw, raw: eq_fields.Draw) -> set[int]:
    """Give the colours raw blends on the ROP where NVK's program fetches them and raw's does not."""
    blend = [f'{0x1360 + 4 * j:x}' for j in range(8)]
    return {
        j
        for j in fetched(nvk) - fetched(raw)
        if raw.registers.get(blend[j], '0') == '1' and nvk.registers.get(blend[j], '0') == '0'
    }


def overlaps(a: tuple, b: tuple) -> bool:
    return a[0] < b[1] and b[0] < a[1] and a[2] < b[3] and b[2] < a[3]


def box_text(b: tuple) -> str:
    return f'{b[0]}-{b[1]}x{b[2]}-{b[3]}'


def registers(nvk: eq_fields.Draw, raw: eq_fields.Draw, tags: set[str]) -> list[str]:
    refused = []
    storage = raw.registers.get('1224', '0') != '0'
    free = set() if storage else fetch_free(nvk, raw)
    for m in sorted(set(nvk.registers) | set(raw.registers)):
        want, got = nvk.registers.get(m, '-'), raw.registers.get(m, '-')
        if want == got:
            continue
        if want == '-' and baseline(m, got, raw):
            tags.add('baseline')
        elif m == '2f4' and want == '-' and got == '1':
            tags.add('blendkill')
        elif storage and (m in STORAGE or per_target_blend(m)):
            tags.add('storage')
        elif blend_target(m) in free:
            tags.add('fetchfree')
        else:
            refused.append(f'register {m} nvk={want} raw={got}')
    return refused


def elided(draws: list[eq_fields.Draw], i: int) -> tuple[str | None, str | None]:
    """Give draw i's elided tag, or why its elision is refused: the boxes written since raw's last barrier meet it."""
    box, written = draws[i].box(), []
    for d in reversed(draws[:i]):
        if d.barriers & ({'PSB'} | SHADER_BARRIER):
            break
        if d.writes_color():
            written.append(d.box())
    met = [b for b in written if overlaps(b, box)]
    if met:
        return None, f'elided barrier meets written pixels {box_text(box)} vs {box_text(met[0])}'
    return 'elided=' + box_text(box) + '|' + ','.join(box_text(b) for b in written), None


def inherited(draws: list[eq_fields.Draw], i: int) -> int | None:
    """Give raw's last draw with a barrier before draw i, where no draw from it to i writes a colour."""
    for j in range(i - 1, -1, -1):
        if draws[j].writes_color():
            return None
        if draws[j].barriers & ({'PSB'} | SHADER_BARRIER):
            return j
    return None


def unfetched(raw: eq_fields.Draw, nvk: eq_fields.Draw) -> str | None:
    """Give the class of a draw whose programs read none of the colours NVK's fetches: it needs no pixel barrier."""
    if not fetched(nvk) or fetched(raw):
        return None
    return 'fetchfree' if fetch_free(nvk, raw) else 'noread'


def barrier_class(
    draws: list[eq_fields.Draw], i: int, nvk: eq_fields.Draw, missing: set[str]
) -> tuple[str | None, str | None]:
    """Give the class of the barriers only NVK puts on draw i, or why none covers them."""
    raw = draws[i]
    if raw.registers.get('1224', '0') != '0' and missing <= PIXEL:
        return 'storage', None
    if 'ELIDED' in raw.barriers and missing <= PIXEL:
        return elided(draws, i)
    if raw.barriers & SHADER_BARRIER and missing <= {'PSB'}:
        return 'shader', None
    if missing <= PIXEL and (last := inherited(draws, i)) is not None:
        return f'inherit={last}', None
    if raw.first and missing <= PASS_START:
        return 'passstart', None
    tag = unfetched(raw, nvk) if missing <= PIXEL else None
    return (tag, None) if tag else (None, f'barriers {sorted(missing)} only in nvk')


def barriers(draws: list[eq_fields.Draw], i: int, nvk: eq_fields.Draw, tags: set[str]) -> list[str]:
    missing = nvk.barriers - draws[i].barriers
    if not missing:
        return []
    tag, why = barrier_class(draws, i, nvk, missing)
    if tag:
        tags.add(tag)
    return [why] if why else []


def kept_set(raw: eq_fields.Draw, before: eq_fields.Draw | None, stage: str) -> bool:
    """Say whether raw's draw binds the same set-0 upload at `stage` as its draw before."""
    upload = raw.stages.get(stage, eq_fields.Stage()).set
    return bool(upload) and before is not None and before.stages.get(stage, eq_fields.Stage()).set == upload


def data(nvk: eq_fields.Draw, raw: eq_fields.Draw, tags: set[str], before: eq_fields.Draw | None = None) -> list[str]:
    keys = sorted(set(nvk.fields) | set(raw.fields))
    refused = [
        f'{k} nvk={nvk.fields.get(k)} raw={raw.fields.get(k)}' for k in keys if nvk.fields.get(k) != raw.fields.get(k)
    ]
    for name, want in nvk.stages.items():
        got = raw.stages.get(name, eq_fields.Stage())
        if want.app - got.content and kept_set(raw, before, name):
            tags.add('unread')
        elif want.app - got.content:
            refused.append(f'{name} uniform contents only nvk binds: {sorted(want.app - got.content)}')
        if want.draw != got.draw:
            refused.append(f'{name} root draw parameters nvk={want.draw} raw={got.draw}')
        moved = {k for k, (a, b) in enumerate(zip(want.push, got.push, strict=False)) if a != b}
        if not moved <= PUSH_WORDS:
            refused.append(f'{name} push words {sorted(moved)}')
        elif moved:
            tags.add('push')
    return refused


def compare(nvk: eq_fields.Frame, raw: eq_fields.Frame) -> tuple[list[set], list[str]]:
    """Each raw draw's tags, and every difference no class covers (draw i of each log being the same draw)."""
    if len(nvk.draws) != len(raw.draws):
        return [], [f'draws: nvk {len(nvk.draws)} raw {len(raw.draws)}']
    tags, refused = [], []
    for i, (n, r) in enumerate(zip(nvk.draws, raw.draws, strict=True)):
        t: set[str] = set()
        why = registers(n, r, t) + barriers(raw.draws, i, n, t) + data(n, r, t, raw.draws[i - 1] if i else None)
        tags.append(t)
        refused += [f'draw {i}: {w}' for w in why]
    # m2v initialises a frame's new textures with clears of its own before the first draw
    if [c for c in nvk.early if c in raw.early] != raw.early or nvk.clears != raw.clears:
        refused.append(f'clears: nvk {nvk.early} {nvk.clears} raw {raw.early} {raw.clears}')
    return tags, refused
