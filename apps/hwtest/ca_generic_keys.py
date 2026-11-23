"""The keys of one IPSW's Core Animation generic merge states (fragment flag g), which ui.launch_hitch counts spans of.

ca_generic_keys.py <qc-<build>.json> <out>. QuartzCore makes a generic state only from its offline compilation manifest,
so the manifest's g states are all of them. Each key is the one the runtime files the state under and names in its
long spans (span_marks.h): Pack::fnv of the state's key (make_pack.py state_key) made of its name with the build's own
tables. <out> holds "<key> <name>" lines, the key as 16 hex digits.
"""

import importlib.util
import sys
from pathlib import Path

sys.path.insert(0, str(Path(importlib.util.find_spec('drivers.TegraGM20BMetal.pack.make_pack').origin).parent))
sys.path.insert(0, str(Path(importlib.util.find_spec('drivers.TegraGM20BMetal.pack.ca.caspec').origin).parent))
import caspec
from make_pack import state_key
from packfile import fnv


def generic_keys(tables: caspec.Tables) -> list[tuple[int, str]]:
    """Give each generic state of the manifest as (key, name), in manifest order."""
    names = [m[0] for m in tables.t['manifest'] if m[0][0] == 'P' and caspec.decode(tables, m[0]).flag('g')]
    return [(fnv(state_key(caspec.state_line(tables, name)).encode()), name) for name in dict.fromkeys(names)]


def main() -> None:
    tables_path, out = sys.argv[1:]
    keys = generic_keys(caspec.Tables(tables_path))
    if not keys:
        raise SystemExit(f'{tables_path}: the manifest names no generic state')
    Path(out).write_text(''.join(f'{key:016x} {name}\n' for key, name in keys))


if __name__ == '__main__':
    main()
