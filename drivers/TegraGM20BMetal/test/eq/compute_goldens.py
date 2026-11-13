"""Write computeeq's goldens from raw's dispatches where they differ from NVK's only as ruled, and check a raw log.

usage: compute_goldens.py write <nvk.log> <raw.log> <kernel> <golden>   appends the kernel's row, or refuses
       compute_goldens.py check <raw.log> <kernel> <golden>

One dispatch of a kernel (computeframe raw: an NVK run's record and NVK's program through raw's compute encoder) is
compared field by field as eqcheck.py reads it, its texture headers by format and size (raw's textures are block-linear
by default): grid and block, the QMD, the program, the root table and push words, set 0 and every buffer it reaches.
NVK's draws of its own (m2v's internal copies) and its early clears are not raw's work. Ruled raw's own:
  passstart  CE_COPY, NVK's upload at its submission's start
  placed     ICINV, the instruction caches raw invalidates in a submission that placed a program in the heap
             (the harness places NVK's program for every run)
  slm        WFI before the channel's local memory grows for a kernel that asks for more (raw_slm.h emitSlm)
A golden row: <kernel> <digest of the fields> <barriers> <tags>; a log passes with the same fields and barriers.
"""

import hashlib
import sys
from pathlib import Path

import eq_fields

eqcheck = eq_fields.eqcheck
NVK_ONLY = {'CE_COPY'}
RAW_ONLY = {'ICINV'}


def dispatch(log: str) -> dict:
    """Give the log's one dispatch's fields, its texture headers by format and size."""
    eqcheck.LAYOUTS = True
    events = [e for submission in eqcheck.rename_all(eqcheck.frames(log)) for e in submission]
    found = [eqcheck.fields(e) for e in events if e['head'].startswith('DISPATCH')]
    if len(found) != 1:
        raise SystemExit(f'{log}: {len(found)} dispatches, not one')
    return found[0]


def digest(fields: dict) -> str:
    text = ' '.join(f'{k}={v}' for k, v in sorted(fields.items()) if k not in {'barriers', 'regs'})
    return hashlib.sha256(text.encode()).hexdigest()[:32]


def compare(nvk: dict, raw: dict) -> tuple[set[str], list[str]]:
    """Give the tags of raw's differences from NVK, and every difference no class covers."""
    refused = [f'{k}: nvk {nvk.get(k)} raw {raw.get(k)}' for k in sorted(set(nvk) | set(raw))
               if k not in {'barriers', 'regs'} and nvk.get(k) != raw.get(k)]  # fmt: skip
    tags = set()
    if nvk['barriers'] - raw['barriers']:
        if nvk['barriers'] - raw['barriers'] <= NVK_ONLY:
            tags.add('passstart')
        else:
            refused.append(f'barriers only nvk has: {sorted(nvk["barriers"] - raw["barriers"])}')
    extra = raw['barriers'] - nvk['barriers']
    if 'WFI' in extra and ' slm=0/0 ' not in raw.get('qmd', ''):
        tags.add('slm')
        extra -= {'WFI'}
    if extra <= RAW_ONLY:
        tags.update({'placed'} if extra else set())
    else:
        refused.append(f'barriers only raw has: {sorted(extra)}')
    return tags, refused


def row(kernel: str, raw: dict, tags: set[str]) -> str:
    return '\t'.join([kernel, digest(raw), ','.join(sorted(raw['barriers'])) or '-', ';'.join(sorted(tags)) or '-'])


def check(log: str, kernel: str, golden: str) -> list[str]:
    rows = {r.split('\t')[0]: r.split('\t') for r in Path(golden).read_text().splitlines()}
    if kernel not in rows:
        return [f'{kernel}: no golden']
    want, got = rows[kernel], row(kernel, dispatch(log), set()).split('\t')
    moved = [] if got[1] == want[1] else [f'{kernel}: the dispatch moved']
    return moved + ([] if got[2] == want[2] else [f'{kernel}: barriers {got[2]}, the golden {want[2]}'])


def main() -> int:
    mode, *paths = sys.argv[1:]
    if mode == 'write':
        nvk_log, raw_log, kernel, golden = paths
        raw = dispatch(raw_log)
        tags, refused = compare(dispatch(nvk_log), raw)
        for line in refused:
            print(f'{kernel}: {line}')
        if refused:
            return 1
        with open(golden, 'a') as out:
            out.write(row(kernel, raw, tags) + '\n')
        return 0
    moved = check(*paths)
    for line in moved:
        print(line)
    return 1 if moved else 0


if __name__ == '__main__':
    sys.exit(main())
