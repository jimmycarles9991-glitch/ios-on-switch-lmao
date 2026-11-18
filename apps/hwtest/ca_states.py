"""The corpus's Core Animation states this IPSW makes.

ca_states.py <corpus.log> <QuartzCore default.metallib> <qc-<build>.json> <out>. A state is this IPSW's where its line
names the IPSW's build (os=), its QuartzCore declares both functions and every constant the line sets on them, and the
constants' values are those its QuartzCore makes of the state's name (raw/pack/ca/caspec.py with this IPSW's tables).
iOS 17's path_stroke_* states fail the second on iOS 27, whose QuartzCore has no such functions; most of iOS 17's other
lines fail the third there (a memoryless index past the pass's attachments, another build's blend function codes),
states iOS 27 never makes.
"""

import importlib.util
import re
import sys
from pathlib import Path

from tools.gpu.shaders import metallib

# caspec imports its iOS 27 half (caspec27) by its bare name, so ca/ goes on the path and caspec is imported as caspec
sys.path.insert(0, str(Path(importlib.util.find_spec('drivers.TegraGM20BMetal.raw.pack.ca.caspec').origin).parent))
import caspec

PREFIX = 'R ok=1 label=com.apple.coreanimation.'
OS_BUILDS = re.compile(r' os=(\S+)')


def stage(segment: str, tag: str) -> tuple[str, set[str]] | None:
    """Return a ` | v ...` or ` | f ...` segment's function and the constants it sets."""
    tokens = segment.split()
    if len(tokens) < 2 or tokens[0] != tag:
        return None
    constants = {t.split(':', 1)[0] for t in tokens[2:] if not t.startswith('lib=') and ':' in t}
    return tokens[1], constants


def made_here(line: str, declared: dict[str, set[str]]) -> bool:
    """Whether the QuartzCore whose functions declare `declared` (function -> its constants) makes this state."""
    parts = line.split(' | ')
    if len(parts) < 3:
        return False
    for segment, tag in ((parts[1], 'v'), (parts[2], 'f')):
        found = stage(segment, tag)
        if found is None or found[0] not in declared or not found[1] <= declared[found[0]]:
            return False
    return True


def tagged(line: str, build: str) -> bool:
    """Whether the line names `build` among the OS builds it is a state of."""
    tag = OS_BUILDS.search(line.split(' | ', 1)[0])
    return bool(tag) and build in tag[1].split(',')


def states(corpus: str, library: bytes, tables: caspec.Tables) -> list[str]:
    """Return the corpus's Core Animation lines that `library` (a QuartzCore default.metallib) with `tables` makes."""
    functions = metallib.functions(library)
    if functions is None:
        raise ValueError('not a Metal library')
    declared = {f.name: {c.name for c in f.constants} | {f'#{c.index}' for c in f.constants} for f in functions}
    return [
        line
        for line in corpus.splitlines()
        if line.startswith(PREFIX)
        and tagged(line, tables.t['build'])
        and made_here(line, declared)
        and caspec.made_of_name(tables, line)
    ]


def main() -> None:
    corpus, library, tables, out = sys.argv[1:]
    kept = states(Path(corpus).read_text(), Path(library).read_bytes(), caspec.Tables(tables))
    if not kept:
        sys.exit(f"{library} makes none of the corpus's Core Animation states")
    Path(out).write_text(''.join(line + '\n' for line in kept))


if __name__ == '__main__':
    main()
