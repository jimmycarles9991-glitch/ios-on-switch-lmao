#!/usr/bin/env python3
"""The symbols a binary still needs from m2v: link it without m2v's libraries and list what nothing defined.

usage (under xcode_run, so clang++ and nm are the pinned Xcode's):
  m2v_link_edges.py --out <edges.txt> --binary <linked> -- clang++ <the binary's link line, m2v's libraries left out>

The link runs with -undefined dynamic_lookup, so it gets as far as the linker goes (dead stripping included) and every
symbol it could not resolve stays an import the loader would have to find. Each line of <edges.txt> is one such symbol
(Mach-O name without its leading underscore) and, tab-separated, the objects of the link line that reference it. A link
that fails anyway, and one the linker refuses dynamic_lookup for, fails here with the linker's output.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

LOOKED_UP = re.compile(r'\(undefined\) (?:weak )?external (\S+) \(dynamically looked up\)')


def run(command: list[str]) -> str:
    done = subprocess.run(command, capture_output=True, text=True, check=False)
    if done.returncode:
        sys.exit(f'{command[0]} failed ({done.returncode}):\n{done.stdout}{done.stderr}')
    return done.stdout


def plain(symbol: str) -> str:
    return symbol.removeprefix('_')


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('link', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    link = args.link[1:] if args.link[:1] == ['--'] else args.link
    run([*link, '-Wl,-undefined,dynamic_lookup', '-o', str(args.binary)])
    missing = {m.group(1) for m in map(LOOKED_UP.search, run(['nm', '-m', str(args.binary)]).splitlines()) if m}
    users: dict[str, set[str]] = {symbol: set() for symbol in missing}
    for path in (Path(a) for a in link if a.endswith(('.o', '.a'))):
        for symbol in run(['nm', '-u', '-j', str(path)]).split():
            if symbol in users:
                users[symbol].add(path.name.removesuffix('.o'))
    lines = [f'{plain(s)}\t{",".join(sorted(users[s])) or "-"}' for s in sorted(missing, key=plain)]
    args.out.write_text(''.join(line + '\n' for line in lines))


if __name__ == '__main__':
    main()
