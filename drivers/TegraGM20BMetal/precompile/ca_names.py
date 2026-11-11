#!/usr/bin/env python3
"""Merge the Core Animation states a device's logs name as missing from the pack into precompile/ca-names/<build>.txt.

usage: ca_names.py <ca-names/<build>.txt> <decoded log>...   (idevicesyslog's or `log show`'s text)

A state.unpacked refusal names a Core Animation state by its label alone, com.apple.coreanimation.<name>. The m2v
journals hold no such state: raw makes Core Animation's states, and the harvest reads only the caches of the system
containers it opens. The name is enough, as QuartzCore makes the state of a name (raw/pack/ca/caspec.py), and the pack
generator adds each name of the IPSW kind's file to its Core Animation corpus tier (raw/pack/rawpack.py ca_states).
The file keeps what it had: one name a line, sorted.
"""

import re
import sys
from pathlib import Path

UNPACKED = re.compile(r' state\.unpacked: .*\bcom\.apple\.coreanimation\.([A-Za-z0-9_]+)')


def names(logs: list[Path]) -> set[str]:
    found = set()
    for log in logs:
        with log.open(errors='replace') as f:
            found |= {m.group(1) for line in f if (m := UNPACKED.search(line))}
    return found


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    had = set(out.read_text().split()) if out.exists() else set()
    merged = had | names([Path(p) for p in sys.argv[2:]])
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(''.join(name + '\n' for name in sorted(merged)))
    print(f'{out}: {len(merged)} names, {len(merged - had)} new')


if __name__ == '__main__':
    main()
