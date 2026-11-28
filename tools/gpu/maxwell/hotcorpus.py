#!/usr/bin/env python3
"""The hot pack's corpus: the split corpus lines of the states devices drew, for a sample pack (gpu/seed:defs.bzl).

usage: hotcorpus.py --split-dir <dir> --qc <QuartzCore default.metallib> --corpus <the corpus step's output>
                    [--ca-names <ca-names/<build>.txt>] --out <file>

<dir> holds the split step's bucket<i>.log files and the corpus step's output (tools/gpu/make_pack.py corpus, split).
Kept: the warm-up line first, every compute dispatch shape, every state of another framework's library but the
generator's tier-1 closure, and the Core Animation states of the harvested, corpus and demand tiers (make_pack.py
ca_states). Left out: our own apps' states and the tiers no device named (manifest, variants, blend, fetch, closure),
which make up most of the full pack and are drawn rarely if ever.
"""

import argparse
import sys
from pathlib import Path

PACK = Path(__file__).resolve().parents[3] / 'drivers/TegraGM20BMetal/pack'
PREFERENCES = Path(__file__).resolve().parents[1] / 'stage_format.py'
sys.path.insert(0, str(PACK))
sys.path.insert(0, str(PACK / 'ca'))
import caspec  # noqa: E402
import deepcolour  # noqa: E402
import make_pack  # noqa: E402

DRAWN_TIERS = ('harvested', 'corpus', 'demand')


def tables_for(qc: Path) -> Path:
    import hashlib  # noqa: PLC0415
    import json  # noqa: PLC0415

    digest = hashlib.sha256(qc.read_bytes()).hexdigest()
    for path in sorted((PACK / 'ca').glob('qc-*.json')):
        if json.loads(path.read_text()).get('metallib_sha256') == digest:
            return path
    raise SystemExit(f'no QuartzCore tables for {digest[:16]}')


def drawn_ca_keys(qc: Path, corpus: Path, names: list[str]) -> set[str]:
    cif10 = deepcolour.cif10(PREFERENCES)
    keys = set()
    for tier, lines in make_pack.ca_states(tables_for(qc), corpus, names, cif10):
        if tier in DRAWN_TIERS:
            keys.update(make_pack.state_key(line) for line in lines)
    return keys


def hot_lines(buckets: list[Path], ca_keys: set[str]) -> list[str]:
    warm, kept = '', []
    for bucket in buckets:
        lines = bucket.read_text().splitlines(keepends=True)
        warm = warm or lines[0]
        for line in lines[1:]:
            if line.startswith('D '):
                kept.append(line)
            elif not line.startswith('R ') or make_pack.APP_LIBRARY.search(line) or ' tier=1 ' in line:
                continue
            elif caspec.corpus_fields(line)[0]:
                if make_pack.state_key(line) in ca_keys:
                    kept.append(line)
            else:
                kept.append(line)
    seen = set()
    return [warm] + [line for line in kept if not (line in seen or seen.add(line))]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--split-dir', type=Path, required=True)
    ap.add_argument('--qc', type=Path, required=True)
    ap.add_argument('--corpus', type=Path, required=True)
    ap.add_argument('--ca-names', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    names = a.ca_names.read_text().split() if a.ca_names else []
    buckets = sorted(a.split_dir.glob('bucket*.log'), key=lambda p: int(p.stem.removeprefix('bucket')))
    lines = hot_lines(buckets, drawn_ca_keys(a.qc, a.corpus, names))
    a.out.write_text(''.join(lines))
    print(f'hotcorpus: {len(lines)} lines')


if __name__ == '__main__':
    main()
