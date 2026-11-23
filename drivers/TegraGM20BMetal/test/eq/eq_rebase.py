"""Hold a raw log to the goldens of an earlier raw where NVK can no longer be run, and rule why each draw moved.

A draw that moved from its golden is refused unless one of these says why:

  form       a fetching state the golden drew in its storage form, drawn now by another of its forms the rules take:
             its barrier twin a triangle at a time (rule fetch.twin, twin_rule.h) or its fetch-free form (rule
             fetch.blend, fetch_rungs.h); its programs, registers and barriers are that form's, its geometry and
             targets the golden's wherever the golden keeps them field by field
  pushword   a draw whose golden already differs from NVK's in push words only one driver's programs read (eq_rules.py
             push, words 17 and 18), every draw of its pass since one that took another form one too: those words hold
             what the draws before left there, which raw's programs never read; nothing but its stages moved wherever
             the golden keeps it field by field
  opening    a pass's first draw opened by a relaxed form (barrier_forms.h): the pixel barrier where every
             dependency is a held cell (barrier.pass_pixel), or nothing behind a job's planner entry
             (barrier.entry_no_drain), in place of the drain's wait and shader-cache flush
  follows    a draw after a draw of its pass that took another form: the shader barrier that ordered it after that
             draw's storage writes goes with them
  tscrenumber
             a draw whose sampler headers sit at other indices (headers placed in another order), seen where it binds a
             set again as a uniform buffer, whose words nvdec hashes as they are: proven against a log that keeps the
             golden, every line of the two logs alike but uniform buffers' word hashes, and every word that differs a
             sampler-header index of the same texture header whose sampler header holds the same words

A draw equal to its golden keeps its golden's tags; its barriers may only grow. `moved` below names the fields the
golden keeps one by one (its sampled draws) that moved.
"""

import re

import eq_fields

STORAGE_TAG = 'storage'
FORMS = ('twin', 'fetchfree')
DRAIN = {'WFI', 'SHINV', 'CBINV', 'ICINV', 'TEXINV'}
SHADER_FLUSH = {'SHINV', 'TEXINV', 'WFI'}
FIXED = ('draw', 'vb', 'rt', 'zt')  # a form keeps the state's geometry and targets


def storage_form(d: eq_fields.Draw) -> bool:
    return d.registers.get('1224', '0') != '0'


def form(golden_tags: set[str], d: eq_fields.Draw, moved: list[str]) -> tuple[str | None, str | None]:
    """Give the form tag of a draw whose golden was its storage form, or why its move is not one."""
    if STORAGE_TAG not in golden_tags or storage_form(d):
        return None, None
    fixed = [k for k in moved if k.startswith(FIXED)]
    if fixed:
        return None, f'took another form and moved {fixed}'
    return ('twin' if d.barriers & {'PSB', 'ELIDED'} else 'fetchfree'), None


def pushword(
    draws: list[eq_fields.Draw], i: int, formed: set[int], pushed: set[int], moved: list[str]
) -> tuple[str | None, str | None]:
    """Give 'pushword' for draw i, or why its move is more than its push words'. `pushed`: the draws tagged push."""
    more = [k for k in moved if not k.startswith('stage')]
    if i not in pushed or more:
        return None, f'moved {more}' if more else None
    for j in range(i - 1, -1, -1):
        if draws[j + 1].first:
            return None, None
        if j in formed:
            return 'pushword', None
        if j not in pushed:
            return None, None
    return None, None


def opening(d: eq_fields.Draw, missing: set[str]) -> str | None:
    if d.first and missing <= DRAIN and ('PSB' in d.barriers or 'WFI' in missing):
        return 'opening'
    return None


def follows(draws: list[eq_fields.Draw], i: int, formed: set[int], missing: set[str]) -> str | None:
    if not missing <= SHADER_FLUSH:
        return None
    for j in range(i - 1, -1, -1):
        if j in formed:
            return 'follows'
        if draws[j].first:
            return None
    return None


UNIFORM = re.compile(r'^(\s*cb\d+ type=5 size=\w+ data=)[0-9a-f]+(:.*)$')
TEXTURE_BITS = 0xFFFFF


def uniforms(log: list[str]) -> list[tuple[int, int]]:
    """Give the stage and slot of each uniform buffer line of a log, in its order."""
    out, stage = [], -1
    for line in log:
        text = line.strip()
        if text.startswith('stage'):
            stage = int(text.split()[0][len('stage') :])
        elif UNIFORM.match(line):
            out.append((stage, int(text.split()[0][len('cb') :])))
    return out


def dumped(words: list[str]) -> list[tuple[int, int, list[int], dict[int, str]]]:
    """Give eq_capture.cpp's EQ_UBO_WORDS lines: stage, slot, words, and the sampler header each handle word names."""
    out = []
    for line in words:
        head, _, named = line.partition(' |')
        _, stage, slot, *values = head.split()
        headers = dict(entry.split(':') for entry in named.split())
        out.append((int(stage), int(slot), [int(v, 16) for v in values], {int(k): h for k, h in headers.items()}))
    return out


def tsc_renumber(was_log: list[str], was_words: list[str], log: list[str], words: list[str]) -> list[str]:
    """Give why two runs' logs differ in more than sampler-header indices, [] where they do not."""
    if len(was_log) != len(log):
        return [f'tscrenumber: logs of {len(was_log)} and {len(log)} lines']
    order = uniforms(log)
    was_dumps, dumps = dumped(was_words), dumped(words)
    if uniforms(was_log) != order or [d[:2] for d in was_dumps] != order or [d[:2] for d in dumps] != order:
        return ["tscrenumber: the word dumps do not list the logs' uniform buffers"]
    refused, k = [], 0
    for a, b in zip(was_log, log, strict=True):
        uniform = bool(UNIFORM.match(b))
        if a != b and (not uniform or UNIFORM.sub(r'\1\2', a) != UNIFORM.sub(r'\1\2', b)):
            return [f"tscrenumber: {b.strip()[:100]} moved, which is no uniform buffer's word hash"]
        if a != b:
            refused += renumbered_words(k, was_dumps[k], dumps[k])
        k += uniform
    return refused


def renumbered_words(k: int, was: tuple, now: tuple) -> list[str]:
    """Give why uniform buffer k's words differ in more than sampler-header indices."""
    (_, _, was_words, was_headers), (_, _, words, headers) = was, now
    if len(was_words) != len(words):
        return [f'tscrenumber: uniform buffer {k} of {len(was_words)} and {len(words)} words']
    refused = []
    for j, (a, b) in enumerate(zip(was_words, words, strict=True)):
        if a == b:
            continue
        same_header = j in was_headers and j in headers and was_headers[j] == headers[j]
        if a & TEXTURE_BITS != b & TEXTURE_BITS or not same_header:
            refused.append(f'tscrenumber: uniform buffer {k} word {j} {a:x} -> {b:x} is no sampler header renumbered')
    return refused
