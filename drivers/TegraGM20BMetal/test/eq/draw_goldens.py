"""Write eq's per-draw goldens from raw's frame where it differs from NVK's only as eq_rules.py allows, and check a log.

The goldens are written while NVK can still be run; a raw log is then held to them alone.

usage: draw_goldens.py write <nvk.log> <raw.log> <golden>   refuses where raw differs from NVK in a way no rule allows
       draw_goldens.py check <raw.log> <golden>

A golden line per item of the frame (eq_fields.py's normalisation):
  C <early clears> <clears>              the frame's clears before its first draw and after, sha256/16 each
  E <i> <digest> <barriers> <tags>       a draw's fields (sha256/16 of all), its barriers, and how it differs from NVK's
  F <i> <field> <hash>                   each field's own hash, for one draw in SAMPLED
A log passes where every draw has its golden's fields and at least its barriers, and leaves a barrier out (ELIDED) only
where the golden's does, its scissor still meeting no pixel written since the last barrier.
"""

import hashlib
import sys
from pathlib import Path

import eq_fields
import eq_rules

SAMPLED = 8


def digest(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()[:32]


def joined(items: list[str]) -> str:
    return ','.join(digest(c) for c in items) or '-'


def rows_of(frame: eq_fields.Frame, tags: list[set]) -> list[list[str]]:
    rows = [['C', joined(frame.early), joined(frame.clears)]]
    for i, d in enumerate(frame.draws):
        fields = eq_fields.digest_fields(d)
        whole = digest(' '.join(f'{k}={v}' for k, v in fields.items()))
        rows.append(['E', str(i), whole, ','.join(sorted(d.barriers)) or '-', ';'.join(sorted(tags[i])) or '-'])
        if int(digest(str(i)), 16) % SAMPLED == 0:
            rows += [['F', str(i), k, digest(v)] for k, v in fields.items()]
    return rows


def read(golden: str) -> list[list[str]]:
    return [line.split('\t') for line in Path(golden).read_text().splitlines()]


def differences(want: list[list[str]], log: str) -> list[str]:
    frame = eq_fields.frame(log)
    got = rows_of(frame, [set()] * len(frame.draws))
    moved = [] if got[0] == want[0] else ['clears moved']
    events = {r[1]: r for r in want if r[0] == 'E'}
    have = {r[1]: r for r in got if r[0] == 'E'}
    if len(events) != len(have):
        return [*moved, f'draws: golden {len(events)} log {len(have)}']
    fields = {(r[1], r[2]): r[3] for r in want if r[0] == 'F'}
    for i, row in events.items():
        g = have[i]
        if g[2] != row[2]:
            now = eq_fields.digest_fields(frame.draws[int(i)])
            named = [k for (j, k), h in fields.items() if j == i and h != digest(now.get(k, ''))]
            moved.append(f'draw {i}: moved {named or ""}')
        need = set() if row[3] == '-' else set(row[3].split(','))
        made = frame.draws[int(i)].barriers
        if not need - {'ELIDED'} <= made:
            moved.append(f'draw {i}: barriers {sorted(made)}, the golden {sorted(need)}')
        if 'ELIDED' in made:
            if 'ELIDED' not in need:
                moved.append(f'draw {i}: a barrier left out where the golden has none left out')
            elif why := eq_rules.elided(frame.draws, int(i))[1]:
                moved.append(f'draw {i}: {why}')
    return moved


def main() -> int:
    mode, *paths = sys.argv[1:]
    if mode == 'write':
        nvk, raw, golden = paths
        n, r = eq_fields.frame(nvk), eq_fields.frame(raw)
        tags, refused = eq_rules.compare(n, r)
        for line in refused[:60]:
            print(line)
        if refused:
            print(f'{len(refused)} differences no rule allows: no golden written')
            return 1
        rows = rows_of(r, tags)
        Path(golden).write_text(''.join('\t'.join(row) + '\n' for row in rows))
        print(f'{len(r.draws)} draws -> {golden}')
        return 0
    log, golden = paths
    want = read(golden)
    moved = differences(want, log)
    for line in moved[:40]:
        print(line)
    print(f'{sum(row[0] == "E" for row in want)} golden draws: {len(moved)} differences')
    return 1 if moved else 0


if __name__ == '__main__':
    sys.exit(main())
