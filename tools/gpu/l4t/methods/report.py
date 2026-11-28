import argparse
import csv
from collections import defaultdict
from pathlib import Path

from decode import METHODS, draw_channel


def query(directory: Path) -> list[str]:
    return [
        line
        for line in (directory / 'run.log').read_text().splitlines()
        if line.startswith(('coverageReductionMode', 'combination', 'unsupported'))
    ]


def unsupported(first: Path, second: Path) -> bool:
    rows = query(first)
    if not rows or rows != query(second):
        raise ValueError('coverage-reduction query differed')
    if 'unsupported combination mode=1 raster=4 depth=4 color=1' not in rows:
        return False
    for case in (first, second):
        manifest = case / 'submit.tsv'
        if manifest.exists() and any(
            line.endswith('\t1') for line in manifest.read_text().splitlines() if line.startswith('submit\t')
        ):
            raise ValueError('unexpected application submission for unavailable TRUNCATE mode')
    return True


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('capture', type=Path)
    parser.add_argument('repeat', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    first, second = (path / 'draws' for path in (args.capture, args.repeat))
    if sorted(p.name for p in first.iterdir()) != sorted(p.name for p in second.iterdir()):
        raise ValueError('different case sets')
    args.output.mkdir()
    with (args.output / 'methods.tsv').open('w') as a, (args.output / 'words.tsv').open('w') as b:
        matrix = csv.writer(a, delimiter='\t', lineterminator='\n')
        evidence = csv.writer(b, delimiter='\t', lineterminator='\n')
        matrix.writerow(['case', *(f'{r:04x}' for r in sorted(METHODS))])
        evidence.writerow(
            ['case', 'submit', 'channel', 'phase', 'subchannel', 'header_at', 'header', 'data_at', 'method', 'value']
        )
        for case in sorted(first.iterdir()):
            twin = second / case.name
            if case.name == 'truncate4_1' and unsupported(case, twin):
                continue
            paired = []
            for directory in (case, twin):
                writes, draw = draw_channel(directory)
                paired.append([r for r in writes if r.method in METHODS or r == draw])
            rows, other = paired
            if [r._replace(channel=0) for r in rows] != [r._replace(channel=0) for r in other]:
                raise ValueError(f'capture differs at {case.name}')
            sequence = defaultdict(list)
            for r in rows:
                phase = 'app' if r.application else 'init'
                evidence.writerow(
                    [
                        case.name,
                        r.submit,
                        r.channel,
                        phase,
                        r.subchannel,
                        r.header_at,
                        f'{r.header:08x}',
                        r.data_at,
                        f'{r.method:04x}',
                        f'{r.value:08x}',
                    ]
                )
                state = f'{phase}:{r.value:#x}'
                if not sequence[r.method] or sequence[r.method][-1] != state:
                    sequence[r.method].append(state)
            matrix.writerow([case.name, *(','.join(sequence[m]) or 'absent' for m in sorted(METHODS))])


if __name__ == '__main__':
    main()
