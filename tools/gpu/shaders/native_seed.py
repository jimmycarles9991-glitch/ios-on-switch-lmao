"""Build native record buckets independently of the runtime stack, stamp the seed, then pack it for the device."""

import argparse
import hashlib
import json
import re
import subprocess
import tempfile
from collections import Counter
from pathlib import Path

from tools.gpu.shaders.precompile import DERIVED, place, store_view


def run(command: list[Path | str], work: Path) -> None:
    env = {
        'PATH': '/usr/bin:/bin',
        'HOME': str(work / 'home'),
        'TMPDIR': str(work / 'tmp'),
        'DYLD_LIBRARY_PATH': str(work),
        'MESA_SHADER_CACHE_DISABLE': 'true',
    }
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=3600, check=False)
    if result.returncode:
        raise RuntimeError(f'native seed tool failed ({result.returncode}):\n{(result.stdout + result.stderr)[-6000:]}')


def native_lines(lines: list[str]) -> list[str]:
    return [line for line in lines if line.startswith(('R ', 'D ', 'C ')) and ' ok=1 ' in line]


def split_lines(text: str) -> tuple[list[str], list[str]]:
    observed, marker, derived = text.partition(DERIVED + '\n')
    return native_lines(observed.splitlines()), native_lines(derived.splitlines()) if marker else []


def analysis_artifacts(row: dict, root: Path) -> dict:
    entries = row.get('analysis', [])
    if len(entries) != 2 or {entry.get('reflection') for entry in entries} != {False, True}:
        raise ValueError('native analysis requires both reflection contracts')
    artifacts = {}
    for entry in entries:
        name = entry['name']
        if len(name) != 68 or not name.endswith('.nca') or any(c not in '0123456789abcdef' for c in name[:64]):
            raise ValueError('invalid native analysis key')
        relative = Path(entry['path'])
        if relative.parts != ('native-analysis', name):
            raise ValueError('native analysis path differs from its key')
        path = root / relative
        if path.is_symlink() or not path.is_file() or not 88 < path.stat().st_size <= 88 + 64 * 1024 * 1024:
            raise ValueError('missing or invalid native analysis file')
        data = path.read_bytes()
        if (
            data[:8] != b'SWNCART\0'
            or int.from_bytes(data[8:12], 'little') != 1
            or int.from_bytes(data[12:16], 'little') != 88
            or int.from_bytes(data[16:24], 'little') != len(data) - 88
            or data[24:56] != bytes.fromhex(name[:64])
            or hashlib.sha256(data[88:]).digest() != data[56:88]
        ):
            raise ValueError('native analysis envelope differs')
        artifacts[entry['path']] = {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}
    return artifacts


# raw_program_pages.h kProgramSlotPage: a slot's entry is whole pages, which the seed pack keeps on pages of their own
SLOT_PAGE_BYTES = 16384


def add_record(row: dict, root: Path, stack: str, artifacts: dict) -> None:
    relative = Path(row['artifact'])
    if relative.parts != (f'raw-{stack}', row['name'] + '.rec'):
        raise ValueError('native record path does not match its stack and key')
    path = root / relative
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'missing native record: {relative}')
    if row['artifact'] not in artifacts:
        data = path.read_bytes()
        artifacts[row['artifact']] = {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}
    for slot in row.get('slots', []):
        relative = Path(slot['path'])
        if relative.parts != (f'raw-{stack}', slot['name'] + '.slot'):
            raise ValueError('native program slot path does not match its stack and bytes')
        path = root / relative
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'missing native program slot: {relative}')
        if slot['path'] not in artifacts:
            data = path.read_bytes()
            if len(data) % SLOT_PAGE_BYTES:
                raise ValueError(f'native program slot of part of a page: {relative}')
            artifacts[slot['path']] = {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}


def region_signatures(line: str) -> dict[int, tuple[list[int], bool]] | None:
    match = re.match(r'D ok=1 tg=(\d+),(\d+),(\d+) edge=(\d+),(\d+),(\d+) threads=([01])(?: |$)', line)
    if not match:
        return None
    values = list(map(int, match.groups()))
    local, edge = values[:3], values[3:6]
    if any(not 0 < n <= 1024 or not 0 <= e < n for n, e in zip(local, edge)) or (not values[6] and any(edge)):
        return None
    return {
        mask: ([edge[i] if mask & (1 << i) else local[i] for i in range(3)], any(edge))
        for mask in range(8)
        if all(not mask & (1 << i) or edge[i] for i in range(3))
    }


def operation_rows(records: list[dict]) -> list[tuple[dict, str]]:
    groups = {}
    for index, row in enumerate(records):
        identity = (
            (row['source'], row['line'], row['input'], row['derived'])
            if all(key in row for key in ('source', 'line', 'input', 'derived'))
            else ('legacy', index)
        )
        groups.setdefault(identity, []).append(row)
    operations = []
    for rows in groups.values():
        first = rows[0]
        kind = first.get('kind', 'render')
        if any(row.get('kind', 'render') != kind for row in rows):
            raise ValueError('native operation has inconsistent record kinds')
        if kind != 'compute':
            if len(rows) != 1:
                raise ValueError('duplicate native operation')
        else:
            signatures = region_signatures(first.get('input', ''))
            whole_refusal = len(rows) == 1 and first['status'] != 'made' and 'region' not in first
            if signatures is not None and not whole_refusal:
                actual = {}
                for row in rows:
                    mask = row.get('region')
                    if type(mask) is not int or mask not in signatures or mask in actual:
                        raise ValueError('missing, duplicate or unexpected compute region')
                    shape, mode = signatures[mask]
                    if row.get('local') != shape or row.get('regionMode') is not mode:
                        raise ValueError('compute region shape differs from the original dispatch')
                    key = row.get('key')
                    if key is not None and (
                        f' | tg={shape[0]},{shape[1]},{shape[2]} |' not in key or key.endswith(' | region=1') != mode
                    ):
                        raise ValueError('compute artifact key differs from its region specialization')
                    actual[mask] = row
                if actual.keys() != signatures.keys():
                    raise ValueError('native dispatch lacks an expected region')
            elif len(rows) != 1 or ('input' in first and first['status'] == 'made'):
                raise ValueError('invalid native dispatch provenance')
        statuses = {row['status'] for row in rows}
        status = (
            'made'
            if statuses == {'made'}
            else next(
                (value for value in ('failed', 'missing-library', 'missing-function', 'refused') if value in statuses),
                None,
            )
        )
        if status is None:
            raise ValueError('invalid native operation status')
        operations.append((first, status))
    return operations


def validate_inputs(records: list[dict], inputs: list[tuple[str, list[str], list[str]]]) -> None:
    expected = Counter(
        (source, line, text, derived)
        for source, observed, guesses in inputs
        for derived, lines in ((False, observed), (True, guesses))
        for line, text in enumerate(lines, 1)
    )
    actual = Counter((row['source'], row['line'], row['input'], row['derived']) for row, _ in operation_rows(records))
    if actual != expected:
        raise ValueError('native seed operation provenance differs from its input lines')


def coverage(manifest: dict, expected: int, root: Path, ipsw: str, tools: dict) -> dict:
    if manifest['stack'] != tools['stack'] or manifest['schema'] != 1:
        raise ValueError('native seed and installed compiler identity differ')
    counts: dict[str, Counter] = {'observed': Counter(), 'derived': Counter()}
    variants: dict[str, Counter] = {'observed': Counter(), 'derived': Counter()}
    kinds: dict[str, Counter] = {'render': Counter(), 'compute': Counter(), 'analysis': Counter()}
    artifacts = {}
    for row, status in operation_rows(manifest['records']):
        tier = 'derived' if row['derived'] else 'observed'
        counts[tier][status] += 1
        kind = row.get('kind', 'render')
        if kind not in kinds:
            raise ValueError(f'unknown native record kind: {kind}')
        if tier == 'observed':
            kinds[kind][status] += 1
    for row in manifest['records']:
        variants['derived' if row['derived'] else 'observed'][row['status']] += 1
        if row['status'] != 'made':
            continue
        if row.get('kind') == 'analysis':
            artifacts.update(analysis_artifacts(row, root))
        else:
            add_record(row, root, tools['stack'], artifacts)
    actual = sum(counts['observed'].values())
    if actual != expected:
        raise ValueError(
            f'native seed accounted for {actual} observed render/dispatch/analysis states, expected {expected}'
        )
    return {
        'schema': 1,
        'scope': 'resolved-native-render-dispatch-and-analysis-records',
        'ipsw': ipsw,
        'stack': tools['stack'],
        'nvk_uuid': tools['nvk_uuid'],
        'nvk_build_id': tools['nvk_build_id'],
        'packVersion': manifest['packVersion'],
        'cacheVersion': manifest['cacheVersion'],
        'counts': {key: dict(value) for key, value in counts.items()},
        'kinds': {key: dict(value) for key, value in kinds.items()},
        'variants': {key: dict(value) for key, value in variants.items()},
        'expected_observed': expected,
        'complete': expected > 0 and counts['observed']['made'] == expected,
        'artifacts': artifacts,
    }


def bucket(args: argparse.Namespace, work: Path) -> None:
    place(args.raw / 'native-seed', work / 'native-seed')
    place(args.raw / 'libmock_iokit.dylib', work / 'libmock_iokit.dylib')
    place(args.nvk, work / 'libvulkan_nouveau.dylib')
    store_view(work / 'store', args.root, json.loads(args.store.read_text()), args.embedded)
    observed, derived = split_lines(args.lines.read_text())
    for name, lines in (('observed', observed), ('derived', derived)):
        (work / f'{name}.log').write_text(''.join(line + '\n' for line in lines))
    run(
        [
            work / 'native-seed',
            '--libs',
            work / 'store',
            work / 'libvulkan_nouveau.dylib',
            work / 'cache',
            args.qc.absolute(),
            '0',
            args.out.absolute(),
            work / 'observed.log',
            '--derived',
            work / 'derived.log',
        ],
        work,
    )
    path = args.out / 'manifest.json'
    manifest = json.loads(path.read_text())
    validate_inputs(
        manifest['records'], [(str(work / 'observed.log'), observed, []), (str(work / 'derived.log'), [], derived)]
    )
    for row in manifest['records']:
        row['source'] = args.lines.name
    path.write_text(json.dumps(manifest, sort_keys=True) + '\n')


def restamp_view(bucket: Path, view: Path) -> Path:
    view.mkdir()
    for entry in bucket.iterdir():
        target = view / entry.name
        if entry.name == 'native-analysis':
            target.mkdir()
            # Bazel links tree inputs; the runtime artifact reader requires regular files with O_NOFOLLOW.
            for artifact in entry.iterdir():
                place(artifact, target / artifact.name)
        else:
            target.symlink_to(entry.absolute(), target_is_directory=entry.is_dir())
    return view


def stamp(args: argparse.Namespace, work: Path) -> None:
    place(args.raw / 'native-seed', work / 'native-seed')
    place(args.raw / 'libmock_iokit.dylib', work / 'libmock_iokit.dylib')
    tools = json.loads(args.manifest.read_text())
    run(
        [
            work / 'native-seed',
            '--restamp',
            tools['stack'],
            args.out.absolute(),
            *(restamp_view(path, work / f'bucket-{index}') for index, path in enumerate(args.bucket)),
        ],
        work,
    )
    manifest = json.loads((args.out / 'manifest.json').read_text())
    inputs = [(path.name, *split_lines(path.read_text())) for path in args.lines]
    validate_inputs(manifest['records'], inputs)
    expected = sum(len(observed) for _, observed, _ in inputs)
    report = coverage(manifest, expected, args.out, args.ipsw, tools)
    (args.out / 'coverage.json').write_text(json.dumps(report, sort_keys=True, indent=1) + '\n')


def pack_lists(manifest: dict) -> tuple[dict[str, tuple[int, str]], dict[str, tuple[int, str]]]:
    """Return the made records' and analyses' {name: (tier, path below the stamped seed)}: 0 observed, 1 derived."""
    records: dict[str, tuple[int, str]] = {}
    analyses: dict[str, tuple[int, str]] = {}
    for row in manifest['records']:
        if row['status'] != 'made':
            continue
        if row.get('kind') == 'analysis':
            table, named = analyses, [(entry['name'][:64], entry['path']) for entry in row['analysis']]
        else:
            slots = [(slot['name'], slot['path']) for slot in row.get('slots', [])]
            table, named = records, [(row['name'], row['artifact'])] + slots
        for name, path in named:
            tier, known = int(bool(row['derived'])), table.get(name)
            if known and known[1] != path:
                raise ValueError(f'native seed has two files named {name}')
            table[name] = (min(tier, known[0]) if known else tier, path)
    return records, analyses


def pack(args: argparse.Namespace, work: Path) -> None:
    """Pack the stamped seed as the device reads it: <out>/raw-<stack> and <out>/native-analysis (kits/SwitchSeedPack).

    The records' index names the stack its records were stamped for. The analyses' names the m2v build the join made
    (its index's build): each analysis key already hashes that build's sources, and the device cannot ask m2v for it.
    """
    manifest = json.loads((args.seed / 'manifest.json').read_text())
    records, analyses = pack_lists(manifest)
    header = args.m2v_index.read_bytes()[:56]
    if header[:8] != b'M2VSEED2':
        raise ValueError('the m2v seed index has another layout')
    args.out.mkdir(parents=True, exist_ok=True)
    stack = manifest['stack']
    for directory, width, identity, table in (
        (f'raw-{stack}', 8, int(stack, 16).to_bytes(32, 'little'), records),
        ('native-analysis', 32, header[24:56], analyses),
    ):
        listing = work / f'{directory}.list'
        listing.write_text(
            ''.join(
                f'{tier}\t{name}\t{(args.seed / path).absolute()}\n' for name, (tier, path) in sorted(table.items())
            )
        )
        run([args.tool.absolute(), args.out.absolute() / directory, str(width), identity.hex(), listing], work)


def check(args: argparse.Namespace) -> None:
    report = json.loads((args.seed / 'coverage.json').read_text())
    if args.strict and not report['complete']:
        raise ValueError(f'native release render/dispatch/analysis seed incomplete: {report["counts"]["observed"]}')
    args.out.write_text(
        json.dumps({key: value for key, value in report.items() if key != 'artifacts'}, sort_keys=True) + '\n'
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    steps = parser.add_subparsers(dest='step', required=True)
    for step, names in (
        ('bucket', ('raw', 'nvk', 'root', 'store', 'qc', 'lines', 'out')),
        ('stamp', ('raw', 'manifest', 'out')),
        ('pack', ('seed', 'm2v-index', 'tool', 'out')),
        ('check', ('seed', 'out')),
    ):
        command = steps.add_parser(step)
        for name in names:
            command.add_argument('--' + name, type=Path, required=True)
    steps.choices['bucket'].add_argument('--embedded', type=Path, action='append', default=[])
    steps.choices['stamp'].add_argument('--bucket', type=Path, action='append', default=[])
    steps.choices['stamp'].add_argument('--lines', type=Path, action='append', default=[])
    steps.choices['stamp'].add_argument('--ipsw', required=True)
    steps.choices['check'].add_argument('--strict', action='store_true')
    args = parser.parse_args()
    if args.step == 'check':
        check(args)
        return
    with tempfile.TemporaryDirectory(prefix='_native_seed_', dir=Path.cwd()) as directory:
        work = Path(directory)
        (work / 'home').mkdir()
        (work / 'tmp').mkdir()
        {'bucket': bucket, 'stamp': stamp, 'pack': pack}[args.step](args, work)


if __name__ == '__main__':
    main()
