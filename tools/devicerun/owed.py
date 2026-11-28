"""Every lane's device checks owed, judged by one pull: per lane, its own checks and the checks it names elsewhere.

A lane's entries are its NOTES.md "device checks owed", in that order. Each is a Check (the line or HW Test field it
reads, and the lane's own threshold, when it gave one; INFO rows only show what to read) or a Ref, a check that lives
in another group, or on another lane's branch until that branch is merged.
"""

import re
from dataclasses import dataclass

from tools.devicerun import hwtest
from tools.devicerun.rules import FAIL, INFO, NA, NO_DATA, PASS, REPORT, TIMELINE, Check, Result

KERNEL = 'kernel'
METAL = 'com.brunostjohn.switch.TegraGM20BMetal'
COMPILER = 'com.brunostjohn.switch.shadercompiler'


@dataclass(frozen=True)
class Ref:
    """A check another group declares; `lands`: the branch that adds it, when this one does not have it yet."""

    name: str
    lands: str = ''


def line(lane: str, name: str, contains: str | tuple[str, ...], rule: str = TIMELINE, **kw: object) -> Check:
    """Return a check of one line, the kernel's by default; `process`, `subsystem` and the rest as Check's."""
    kw.setdefault('process', KERNEL)
    return Check(f'owed.{lane}.{name}', f'big-{lane}', contains, rule, **kw)


def row(lane: str, test: str, *fields: str, rule: str = TIMELINE, words: tuple[str, ...] = (), **kw: object) -> Check:
    """HW Test `test`'s line, its numeric `fields` and text `words` named as groups in any order (LogLine.swift)."""
    look = ''.join(rf'(?=.*\b{f}=(?P<{f}>-?[\d.]+)\b)' for f in fields)
    look += ''.join(rf'(?=.*\b{w}=(?P<{w}>\S+))' for w in words)
    name = kw.pop('name', test + ('.' + '.'.join(fields) if fields and rule != TIMELINE else ''))
    return Check(
        f'owed.{lane}.{name}',
        f'big-{lane}',
        f'HWTEST {test} ',
        rule,
        subsystem=hwtest.SUBSYSTEM,
        regex=rf'^HWTEST {re.escape(test)} (?P<verdict>PASS|FAIL|SKIP)\b{look}',
        source=kw.pop('source', hwtest.SOURCE),
        **kw,
    )


def none_of(lane: str, test: str, field: str, **kw: object) -> Check:
    """HW Test `test` with no field matching `field` (a regex) above 0; NO-DATA until the test has run."""
    return Check(
        f'owed.{lane}.{test}.{kw.pop("name", "zero")}',
        f'big-{lane}',
        f'HWTEST {test} ',
        'count == 0',
        subsystem=hwtest.SUBSYSTEM,
        regex=rf'^HWTEST {re.escape(test)} \w+\b.*?\b(?P<field>{field})=(?P<n>-?(?:[1-9][\d.]*|0\.\d*[1-9]))\b',
        show=('field',),
        needs=kw.pop('needs', f'owed.{lane}.{test}'),
        source=kw.pop('source', hwtest.SOURCE),
        **kw,
    )


def checks(entries: list[Check | Ref]) -> list[Check]:
    return [e for e in entries if isinstance(e, Check)]


def lane_verdict(results: list[Result]) -> str:
    verdicts = {r.verdict for r in results}
    if FAIL in verdicts:
        return FAIL
    if PASS in verdicts:
        return PASS
    return INFO if INFO in verdicts else NO_DATA


def lane_row(lane: str, entries: list[Check | Ref], by_check: dict[str, Result]) -> Result:
    results = [by_check[e.name] for e in entries if e.name in by_check]
    later = [e for e in entries if isinstance(e, Ref) and e.name not in by_check]
    counts = [f'{v} {sum(r.verdict == v for r in results)}' for v in (PASS, FAIL, NO_DATA, INFO, NA)]
    observed = ', '.join(c for c in counts if not c.endswith(' 0')) or 'nothing judged'
    failed = [r.check.name for r in results if r.verdict == FAIL]
    if failed:
        observed += ': ' + ' '.join(failed)
    if later:
        observed += '; not on this branch: ' + ', '.join(f'{e.name} (with {e.lands})' for e in later)
    check = Check(f'owed.{lane}', f'big-{lane}', '', REPORT)
    return Result(check, lane_verdict(results) if results else NO_DATA, observed)


def archive_row(boots: list[str]) -> Result:
    """Return the boots the pull holds: a line logged once at boot reads NO-DATA when its boot started before it."""
    observed = f'{len(boots)} boot{"" if len(boots) == 1 else "s"}: ' + '; '.join(boots) if boots else 'no boot'
    return Result(Check('owed.archive', 'big-measure', '', REPORT), INFO, observed)
