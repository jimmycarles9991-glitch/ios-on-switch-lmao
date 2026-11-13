"""The Core Animation states this device's QuartzCore asks for that the raw pack holds by deduction, not by a journal.

ca_asked.py <qc-<build>.json> <stage-tegra-gm20b-metal.py> <out> [<ca-names/<build>.txt>]. Apple's offline compilation
manifest names its states for extended-colour devices; this device's QuartzCore names them as
raw/pack/ca/deepcolour.py gives (asked), from the Core Animation preferences the stager writes. A device's logs name the
states it made at run time (precompile/ca-names/<build>.txt), which the pack's corpus tier makes from their names. <out>
starts with "# cif10=<answer>" and holds each asked state the manifest does not name, then each named one, as R lines,
which gpu.ca.asked_forms makes on the raw facade.
"""

import importlib.util
import sys
from pathlib import Path

sys.path.insert(0, str(Path(importlib.util.find_spec('drivers.TegraGM20BMetal.raw.pack.ca.caspec').origin).parent))
import caset
import caspec
import deepcolour


def asked_lines(tables: caspec.Tables, cif10: bool | None) -> list[str]:
    """Return the R lines of the states the device asks for where the manifest names another."""
    named = {m[0] for m in tables.t['manifest']}
    lines = []
    for name, *_ in tables.t['manifest']:
        if name[0] != 'P' or (not tables.v27 and caspec.decode(tables, name).flag('g')):
            continue
        lines += [
            caspec.state_line(tables, form) for form in deepcolour.asked(tables, name, cif10) if form not in named
        ]
    return lines


def named_lines(tables: caspec.Tables, names: list[str]) -> list[str]:
    """Return the R lines of the states a device's logs named that this QuartzCore makes."""
    return [caspec.state_line(tables, name) for name in names if caset.makes(tables, name)]


def main() -> None:
    tables_path, stager, out, *names = sys.argv[1:]
    tables = caspec.Tables(tables_path)
    cif10 = deepcolour.cif10(Path(stager))
    lines = asked_lines(tables, cif10) + named_lines(tables, Path(names[0]).read_text().split() if names else [])
    answer = 'mobilegestalt' if cif10 is None else str(cif10).lower()
    Path(out).write_text(f'# cif10={answer}\n' + ''.join(line + '\n' for line in dict.fromkeys(lines)))


if __name__ == '__main__':
    main()
