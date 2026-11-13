"""Rewrite an IPSW kind's record goldens and the sample pack's corpus from a full raw pack.

usage: record_goldens.py <write_goldens> <pack dir> <kind> <the pack's split bucket logs>... [--out <dir>]

write_goldens writes testdata/records_<kind>.bin, _sample.tsv and _sample_states.txt from the pack; then the sample
pack's corpus, testdata/records_<kind>_sample.corpus, is the pack's split lines of the sampled states. --out writes the
four files to <dir> instead of the workspace's testdata (`bazel run ... -- --out <dir>`), for a run in a checkout
other than the one that takes them. A bucket begins
with the warm-up state (rawpack.py split_buckets), which the probe draws first, so the first bucket's first line leads.
Then every R line whose state key, less a variant's name, hashes (packfile.fnv of rawpack.state_key) to a sampled state,
or to no state of the pack, and every D line (the kernels' dispatch shapes, a few dozen), each once.

A line's text names its state's key only where the descriptor made of it prints the same: a bundle library's state is
filed under its functions' AIR (runtime/RawRenderKey.h), and some fields come back narrower (an index of -1 as 32
bits). Such a line's key is no state's of the pack, and it is taken whatever state it makes.
"""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'raw/pack'))
import packfile
import rawpack


def pick(states: set[int], known: set[int], logs: list[Path]) -> list[str]:
    lines: list[str] = []
    seen: set[str] = set()
    for n, log in enumerate(logs):
        for i, line in enumerate(log.read_text().splitlines()):
            lead = n == 0 and i == 0
            if not lead and line.startswith('R '):
                # a variant's line (cubic binds) is its state's: its record files under the state's key
                state = packfile.fnv(packfile.VARIANT_NAME.sub('', rawpack.state_key(line)).encode())
                if state not in states and state in known:
                    continue
            elif not lead and not line.startswith('D '):
                continue
            if line not in seen:
                seen.add(line)
                lines.append(line)
    return lines


def main() -> None:
    write_goldens, pack, kind, *outputs = sys.argv[1:]
    testdata = Path(os.environ['BUILD_WORKSPACE_DIRECTORY']) / 'drivers/TegraGM20BMetal/test/records/testdata'
    if '--out' in outputs:
        at = outputs.index('--out')
        testdata = Path(outputs[at + 1])
        del outputs[at : at + 2]
    with tempfile.TemporaryDirectory() as tmp:
        known_file = Path(tmp) / 'pack_states.txt'
        subprocess.run([write_goldens, str(Path(pack) / 'pack.bin'), kind, str(testdata), str(known_file)], check=True)
        known = {int(word, 16) for word in known_file.read_text().split()}
    logs = sorted(
        (Path(p) for p in outputs if re.fullmatch(r'bucket\d+\.log', Path(p).name)),
        key=lambda p: int(p.stem.removeprefix('bucket')),
    )
    if not logs:
        sys.exit("no split bucket logs among the pack's report outputs")
    states = {int(word, 16) for word in (testdata / f'records_{kind}_sample_states.txt').read_text().split()}
    corpus = testdata / f'records_{kind}_sample.corpus'
    lines = pick(states, known, logs)
    corpus.write_text(''.join(line + '\n' for line in lines))
    print(f'{len(lines)} lines for {len(states)} sampled states -> {corpus}')


if __name__ == '__main__':
    main()
