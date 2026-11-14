"""Hold a raw log to the goldens of an earlier raw where NVK can no longer be run, and rule why each draw moved.

A draw that moved from its golden is refused unless one of these says why:

  form       a fetching state the golden drew in its storage form, drawn now by another of its forms the rules take:
             its barrier twin a triangle at a time (rule fetch.twin, raw_twin_rule.h) or its fetch-free form (rule
             fetch.blend, raw_fetch_rungs.h); its programs, registers and barriers are that form's, its geometry and
             targets the golden's wherever the golden keeps them field by field
  pushword   a draw whose golden already differs from NVK's in push words only one driver's programs read (eq_rules.py
             push, words 17 and 18), every draw of its pass since one that took another form one too: those words hold
             what the draws before left there, which raw's programs never read; nothing but its stages moved wherever
             the golden keeps it field by field
  opening    a pass's first draw opened by a relaxed form (raw_barrier_forms.h): the pixel barrier where every
             dependency is a held cell (barrier.pass_pixel), or nothing behind a job's planner entry
             (barrier.entry_no_drain), in place of the drain's wait and shader-cache flush
  follows    a draw after a draw of its pass that took another form: the shader barrier that ordered it after that
             draw's storage writes goes with them

A draw equal to its golden keeps its golden's tags; its barriers may only grow. `moved` below names the fields the
golden keeps one by one (its sampled draws) that moved.
"""

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
