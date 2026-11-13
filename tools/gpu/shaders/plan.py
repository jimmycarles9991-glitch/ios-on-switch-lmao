"""Split the seed's work into buckets: the corpus's lines and, past depth `corpus`, the pipelines derive.py adds.

usage: plan.py --corpus <resolved corpus.log> --root <library root> [--data-dir <dir>]... [--data <sha256>.metallib]...
               --qc <metallib> --qc-sha256 <pin> --depth <depth> --buckets <dir> --count N --store <store.json>
               --report <plan.json> [--apps N --base-root <root> --base-data-dir <dir> --base-store <store.json>]

Every line goes to the bucket its program (its stages) hashes to, so a program's pipelines are made in one bucket and
a corpus edit rewrites only the buckets of the programs it adds lines to: the others' bucket-NNN.log keep their bytes,
and Bazel keeps their results. store.json names every library made from bytes by its sha256 (the bucket steps link
data/<sha256>.metallib to it without reading the libraries again).

--apps: a line with a stage of an app's library (below /Applications) goes to one of N more buckets after the --count
others, which read the full library root; the others read --base-root, the same libraries without the apps', through
--base-store. An HW Test edit then changes only the apps' buckets' inputs.
"""

import argparse
import hashlib
import json
import sys
from collections import Counter
from pathlib import Path

from tools.gpu.shaders import derive
from tools.gpu.shaders.journal import kernel_links, parse_kernel, parse_render, parse_specialization
from tools.gpu.shaders.metallib import Function, functions

DEPTHS = ('corpus', 'wide', 'deep')
DERIVED = '# derived'
APPLICATION = 'lib=/Applications/'


def libraries(root: Path, data: list[Path]) -> tuple[dict[str, list[Function]], dict[str, str]]:
    """Map each library's journal spelling to its functions, and each one's sha256 to its file."""
    index, by_sha = {}, {}
    for path in sorted(p for p in root.rglob('*') if p.is_file()):
        content = path.read_bytes()
        by_sha[hashlib.sha256(content).hexdigest()] = str(path)
        found = functions(content)
        if found:
            index['/' + str(path.relative_to(root))] = found
    for path in data:
        content = path.read_bytes()
        digest = hashlib.sha256(content).hexdigest()
        by_sha[digest] = str(path)
        found = functions(content)
        if found:
            index['data:' + digest] = found
    return index, by_sha


def lines_for(depth: str, corpus: list[str], index: dict[str, list[Function]]) -> dict[str, list[str]]:
    """Return the lines of each tier the depth takes."""
    tiers = {'corpus': [line for line in corpus if line[:2] in ('S ', 'R ', 'C ', 'D ')]}
    if depth == 'corpus':
        return tiers
    renders = [r for r in map(parse_render, corpus) if r]
    tiers['kernels'] = derive.kernels(index, [line for line in corpus if not line.startswith('D ')])
    tiers['targets'] = [r.text() for r in derive.targets(renders)]
    if depth == 'deep':
        tiers['neighbours'] = [r.text() for r in derive.neighbours(renders)]
    return tiers


def _identity(line: str) -> tuple:
    if line.startswith('D '):
        return ('D', line)
    render = parse_render(line)
    if render:
        return ('R', *render.key())
    kernel = parse_kernel(line)
    if kernel:
        return ('C', *kernel, kernel_links(line))
    return ('S', parse_specialization(line))


def _program(line: str) -> str:
    render = parse_render(line)
    if render:
        return ' | '.join(
            [
                render.vertex.text(),
                render.fragment.text() if render.fragment else '-',
                *(tag + ' ' + stage.text() for tag, stage in render.linked),
            ]
        )
    kernel = parse_kernel(line)
    if kernel:
        return ' | '.join([kernel[1].text(), *(tag + ' ' + stage.text() for tag, stage in kernel_links(line))])
    stage = parse_specialization(line)
    return stage.text() if stage else line


def bucket(tiers: dict[str, list[str]], count: int, apps: int = 0) -> tuple[list[list[str]], Counter]:
    """Each distinct line once (the first tier that has it), in the bucket of its program (its stages).

    With `apps`, a program with an app's library goes to one of the last `apps` buckets instead.
    """
    buckets: list[list[str]] = [[] for _ in range(count + apps)]
    derived: list[list[str]] = [[] for _ in range(count + apps)]
    seen, kept = set(), Counter()
    for tier, lines in tiers.items():
        for line in lines:
            identity = _identity(line)
            if identity in seen:
                continue
            seen.add(identity)
            kept[tier, line[0]] += 1
            program = _program(line)
            number = int.from_bytes(hashlib.sha256(program.encode()).digest()[:4], 'little')
            into = buckets if tier == 'corpus' else derived
            into[count + number % apps if apps and APPLICATION in program else number % count].append(line)
    return [[*s, DERIVED, *d] for s, d in zip(buckets, derived, strict=True)], kept


def main() -> None:
    """Write the buckets, store.json and the plan's report."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--corpus', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--data', type=Path, action='append', default=[])
    parser.add_argument('--data-dir', type=Path, action='append', default=[])
    parser.add_argument('--qc', type=Path, required=True)
    parser.add_argument('--qc-sha256', required=True)
    parser.add_argument('--depth', choices=DEPTHS, required=True)
    parser.add_argument('--buckets', type=Path, required=True)
    parser.add_argument('--count', type=int, required=True)
    parser.add_argument('--store', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--input-report', type=Path, action='append', default=[])
    parser.add_argument('--apps', type=int, default=0)
    parser.add_argument('--base-root', type=Path)
    parser.add_argument('--base-data-dir', type=Path, action='append', default=[])
    parser.add_argument('--base-store', type=Path)
    args = parser.parse_args()
    if hashlib.sha256(args.qc.read_bytes()).hexdigest() != args.qc_sha256:
        sys.exit(f"{args.qc} is not the IPSW kind's QuartzCore default.metallib ({args.qc_sha256})")
    data = args.data + sorted(p for d in args.data_dir for p in d.glob('*.metallib'))
    index, by_sha = libraries(args.root, data)
    corpus = args.corpus.read_text().splitlines()
    buckets, kept = bucket(lines_for(args.depth, corpus, index), args.count, args.apps)
    args.buckets.mkdir(parents=True, exist_ok=True)
    for i, lines in enumerate(buckets):
        (args.buckets / f'bucket-{i:03d}.log').write_text(''.join(line + '\n' for line in lines))
    args.store.write_text(json.dumps(by_sha, indent=0, sort_keys=True) + '\n')
    if args.base_store:
        base_data = args.data + sorted(p for d in args.base_data_dir for p in d.glob('*.metallib'))
        _, base = libraries(args.base_root, base_data)
        args.base_store.write_text(json.dumps(base, indent=0, sort_keys=True) + '\n')
    report = {
        'depth': args.depth,
        'libraries': len(index),
        'kernels': sum(1 for fs in index.values() for f in fs if f.type == derive.KERNEL),
        'lines': {f'{tier}.{kind}': n for (tier, kind), n in sorted(kept.items())},
        'buckets': [len(b) - 1 for b in buckets],
        'inputs': [json.loads(path.read_text()) for path in args.input_report],
    }
    args.report.write_text(json.dumps(report, indent=1) + '\n')
    print('plan:', json.dumps(report['lines']))


if __name__ == '__main__':
    main()
