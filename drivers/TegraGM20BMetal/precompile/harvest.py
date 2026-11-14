#!/usr/bin/env python3
"""Merge device journals (the frontend's pipelines.log) into the precompile corpus.

A journal has S lines, one per function specialization, R lines, one per render pipeline state, C lines, one per compute
pipeline state, and D lines, one per dispatch shape a compute state took. The corpus has each made once, by any
process, in the order first seen, without the times, threads, costs and callers. The corpus keeps what it had; new
journals only add. Each function names its library (lib=: the file's path, or data:<SHA-256> of a library made from
bytes); a line from before the journal named libraries has none, and precompile/libraries.py guesses it by the
function's name.

usage: harvest.py <corpus.log> --build <IPSW build> <pipelines.log>...
           e.g. harvest.py corpus.log --build 24B119 <state>/device-logs/*/m2v-cache/pipelines.log
       harvest.py <corpus.log> --build <IPSW build> --volume <mounted Data volume> [--save <dir>]
                  [--open <container UUID>]... [--open-all]

--build   the OS build the journals were written under (a journal does not say): each render and dispatch line it adds
          names it (os=<build>), as the raw pack keeps a line to the builds it names (tools/gpu/rawpack.py own_build)

--volume  every process's m2v cache on the iPad's Data volume (the plugin's cacheCandidates: a container's
          Library/Caches/m2v, Library/Caches/m2v-<process> or tmp/m2v, tmp/m2v-<process>, a daemon's user cache or
          temp directory), its pipelines.log and pipelines.log.old. Read only, but for one write: a system container's
          Library and Library/Caches are mode 000, so for a container named by --open (default: backboardd's,
          SpringBoard's, PosterBoard's, Preferences' and Spotlight's, by SYSTEM_CACHES' UUIDs or by their container
          manager identifiers, SYSTEM_IDENTIFIERS) or every one with --open-all, each closed directory on the way is
          made u+rx for the read and given its mode back at once (and checked); other closed containers are skipped
          and counted.
--save    also copy each m2v cache found, whole (v1/, msl/, libs/, the journals, makings.log), to <dir>/<where it was>,
          e.g. the boot's log folder. makings.log (m2v 0118, the plugin turns it on in every cache): one line per driver
          record the process wrote (a compile), its name and the making's description; a precompile run with
          METAL2VULKAN_MAKING_LOG=<file> writes the host's, so a seeded making the device made again shows where the two
          descriptions part.
--device  the device data staging directory (default <state>/gpu-device-data/staging; <state> is ~/Library/Application
          Support/switch-ios or $SWITCH_HOME): with --volume, and for every saved cache below --records-from, each
          driver record (the content directory's v1/<content>/<xx>/<name>, m2v's VkPipelineCache data of one pipeline
          making, named by what the making reads and the NVK identity) whose envelope checks out is copied to
          <device>/records/<xx>/<name>, every cache's libs/<SHA-256>.metallib (a library a process made from bytes)
          to <device>/data/, and every msl/<name>.reply (the frontend's kept MTLCompiler reply: Core Image's kernel
          source compiled to AIR) whose envelope checks out to <device>/msl/. The device's own makings: those of draws'
          variants that no creation predicts, and each one the precompile makes otherwise than the device (r8-webgpu:
          1268 of the 1293 records of the 23:17 boot's caches were not in the seed). The seed takes those of its NVK's
          identity, and every reply.
          harvest.py --records-from <dir>... [--device <dir>]: only the records and compile replies of every m2v cache
          below each <dir> (the --save copies, e.g. <state>/device-logs/*/m2v), no corpus.
--pack    harvest.py --pack [--device <dir>]: the staging directory as the build's @gpu_device_data: a tar (sorted, no
          times or owners) of its records and of the libraries made from bytes that the corpus names and the build
          cannot make itself (not one of gpu/apple-libs.sha256's, not the frontend's TwoPlaneStandIn.h: Core Image's
          kernels compiled on the device) and of its compile replies, written to
          <state>/gpu-device-data/m2v-device-<sha256[:16]>.tar; prints the DEVICE_DATA pin for gpu/deps.bzl. The first
          build after the pin copies the tar into the repository cache; no build reads the staging directory.
"""

import hashlib
import io
import os
import plistlib
import re
import shutil
import stat
import sys
import tarfile
from collections.abc import Callable, Iterator, Sequence
from contextlib import contextmanager
from pathlib import Path

from tools.gpu.shaders import replies as compiler_replies

# os=<build>: the OS build the journal was written under, which the raw pack keeps a line to (tools/gpu/rawpack.py
# own_build)
DESCRIPTOR = re.compile(r'(samples|topo|ds|opt|os|c\d|a\d+)=')
OS_BUILD = re.compile(r' os=\S+')
COMPUTE = re.compile(r'(form|opt|max|tgm|icb|depth|abf|mut)=')
# The checkout: bazel run's (also what //deploy:pull_logs runs this in), else the one this file is in.
PROJECT = Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY') or Path(__file__).resolve().parents[3])
# The device's data, outside every checkout (it is Apple-derived, so never tracked): where --pack writes the tar
# @gpu_device_data's first fetch on a machine reads (gpu/deps.bzl), and the staging directory beside it.
DEVICE = (
    Path(os.environ.get('SWITCH_HOME') or Path.home() / 'Library/Application Support/switch-ios') / 'gpu-device-data'
)
# /private/var/containers/Data/System/<UUID>/Library/Caches/m2v of the system containers whose processes kept an m2v
# cache there (the plugin's "device ... ready in" lines, r5d and r6h): their Library and Library/Caches are mode 000.
# The UUIDs are one install's (21H461's, r5d); another install names the same containers otherwise (24B119's boot 15:
# backboardd's CE929FE0-..., SpringBoard's 1809AE07-...), so a container is also known by the identifier its
# container manager metadata gives it.
SYSTEM_CACHES = {
    '6C7A65AF-CB18-421C-A07A-D724B05A7AB6': 'backboardd',
    '532E562F-2888-4C55-9BF8-E6F0B246BBDA': 'SpringBoard',
    '5BDBB9F1-C07A-4290-9D80-EA49A6BA2D68': 'Preferences',
    'AA90E4B6-B0E4-4776-8E70-A4444E65642F': 'Spotlight',
}
SYSTEM_IDENTIFIERS = {
    'com.apple.backboardd',
    'com.apple.springboard',
    'com.apple.PosterBoard',
    'com.apple.Preferences',
    'com.apple.Spotlight',
}
CONTAINER_METADATA = '.com.apple.mobile_container_manager.metadata.plist'
JOURNALS = ('pipelines.log.old', 'pipelines.log')  # oldest first: the frontend renames the journal past 2 MiB
CONTENT = (
    '529a20fd270ababa3e5ec2fe4d1674d93739ba5bcd61861674b4fb6db349af9f'  # m2v's content directory (seed.py's CONTENT)
)
# m2v shader_cache/driver.rs: magic, identity (28 bytes), data length (u64), data, SHA-256
RECORD_MAGIC = b'M2VPPLC\0'
# The frontend's kept MTLCompiler reply (NativeBridgeMetalFrontend.mm NBCompileReplyHeader): "NSC1", the warning and
# metallib lengths, the SHA-256 of both, then both; named by the SHA-256 of the compiler's LC_UUID and the request.
REPLY_MAGIC = b'NSC1'
REPLY_HEADER = 48
REPLY_NAME = re.compile(r'^[0-9a-f]{64}\.reply$')


def normal(line: str, build: str | None = None) -> tuple[str | None, str | None]:
    """Return a journal line's corpus form and the text it is deduplicated by (None for refusals and other lines).

    A render or dispatch line that names no OS build takes `build`.
    """
    if ' ok=1 ' not in line:
        return None, None
    if line.startswith('S '):
        return _normal_specialization(line)
    if line.startswith('C '):
        return _normal_compute(line)
    if line.startswith(('D ', 'R ')) and build and not OS_BUILD.search(line.split(' | ', 1)[0]):
        line = _with_build(line, build)
    if line.startswith('D '):
        return line.rstrip(), line.rstrip()
    if line.startswith('R '):
        return _normal_render(line)
    return None, None


def _with_build(line: str, build: str) -> str:
    head, sep, rest = line.partition(' | ')
    if line.startswith('R '):
        # first among the state's descriptors, where every corpus line has it, whatever the journal put before label=
        tokens = head.split(' ')
        at = next((i for i, t in enumerate(tokens) if DESCRIPTOR.match(t)), len(tokens))
        return ' '.join([*tokens[:at], f'os={build}', *tokens[at:]]) + sep + rest
    return f'{head} os={build}{sep}{rest}'


def _normal_specialization(line: str) -> tuple[str | None, str | None]:
    m = re.search(r' (lib=\S+ )?name=(\S+) k=(.*?)( \| |$)', line)
    if not m:
        return None, None
    lib = m[1] or ''
    return f'S ok=1 {lib}name={m[2]} k={m[3]} | ', f'{lib}{m[2]} {m[3]}'


def _normal_compute(line: str) -> tuple[str | None, str | None]:
    parts = line.split(' | ')
    head = parts[0].split()
    stages = [p for p in parts[1:] if p[:2] in ('k ', 'l ', 'p ')]
    if not stages:
        return None, None
    label = next((t for t in head if t.startswith('label=')), None)
    fields, stages = ' '.join(t for t in head if COMPUTE.match(t)), ' | '.join(stages)
    labelled = f' {label}' if label else ''
    return f'C ok=1 {fields}{labelled} | {stages} | ', fields + ' | ' + stages


def _normal_render(line: str) -> tuple[str | None, str | None]:
    v = line.find(' | v ')
    f = line.find(' | f ', v)
    end = line.find(' | ', f + 5) if f >= 0 else -1
    if v < 0 or f < 0:
        return None, None
    # The functions the state links follow its stages (the frontend's linkedText: " | <vl|vp|fl|fp> <stage>"), then
    # the cubic samplers' binds of a state's variant (" | cubic ...", raw_cubic.h), then the slots its sampled two-plane
    # variant reads two-plane textures at (" | xrs=<slots>", raw_xr_pass.h), then the formats its written variant stores
    # (" | wtex=<slot>:<format>,...", raw_written_textures.h)
    while end >= 0 and (
        line[end + 3 : end + 6] in ('vl ', 'vp ', 'fl ', 'fp ')
        or line[end + 3 : end + 9] == 'cubic '
        or line[end + 3 : end + 7] == 'xrs='
        or line[end + 3 : end + 8] == 'wtex='
    ):
        end = line.find(' | ', end + 3)
    head = line[:v].split()
    label = next((t for t in head if t.startswith('label=')), 'label=-')
    state = ' '.join(t for t in head if DESCRIPTOR.match(t)) + line[v : end if end >= 0 else len(line)]
    return f'R ok=1 {label} {state} | ', state


@contextmanager
def opened(dirs: tuple[Path, ...], log: Callable[[str], None]) -> Iterator[None]:
    """Make each of `dirs` (outermost first) that this user may not enter u+rx while the block runs.

    Each is given its mode back after it. The only write the volume sees.
    """
    restore = []
    try:
        for d in dirs:
            try:
                mode = stat.S_IMODE(os.lstat(d).st_mode)
            except FileNotFoundError:  # nothing deeper either: the block finds no cache
                break
            if mode & stat.S_IRUSR and mode & stat.S_IXUSR:
                continue
            os.chmod(d, mode | stat.S_IRUSR | stat.S_IXUSR)
            restore.append((d, mode))
        yield
    finally:
        for d, mode in reversed(restore):
            os.chmod(d, mode)
            now = stat.S_IMODE(os.lstat(d).st_mode)
            if now != mode:
                log(f'MODE NOT RESTORED: {d} is {now:o}, was {mode:o}')


def readable(d: Path) -> bool:
    return os.access(d, os.R_OK | os.X_OK)


def caches(
    volume: Path, opens: set[str], open_all: bool, _log: Callable[[str], None], explicit: bool = False
) -> tuple[list[tuple[str, Path, tuple[Path, ...]]], list[str]]:
    """Find every m2v cache on the volume, and the closed containers skipped.

    Each cache as (where, m2v directory, the directories to open on the way). Without `explicit` opens (--open), a
    system container of SYSTEM_IDENTIFIERS' is opened as SYSTEM_CACHES' are.
    """
    found, skipped = [], []
    for kind, base in (
        ('System', volume / 'containers/Data/System'),
        ('Application', volume / 'mobile/Containers/Data/Application'),
        ('PluginKitPlugin', volume / 'mobile/Containers/Data/PluginKitPlugin'),
    ):
        for c in _containers(base):
            may_open = open_all or c.name in opens or (not explicit and _identifier(c) in SYSTEM_IDENTIFIERS)
            in_container, closed = _container_caches(f'{kind}/{c.name}', c, may_open)
            found += in_container
            skipped += closed
    for pattern in (
        'tmp/m2v-*',
        'tmp/m2v',
        'mobile/tmp/*/m2v',
        'mobile/Library/Caches/m2v',
        'root/Library/Caches/m2v',
        'folders/*/*/C/m2v',
        'folders/*/*/T/m2v',
    ):
        for m2v in sorted(volume.glob(pattern)):
            if m2v.is_dir() and readable(m2v):
                found.append((str(m2v.relative_to(volume)), m2v, ()))
    return found, skipped


def _identifier(container: Path) -> str | None:
    try:
        with (container / CONTAINER_METADATA).open('rb') as f:
            return plistlib.load(f).get('MCMMetadataIdentifier')
    except (OSError, plistlib.InvalidFileException, AttributeError):
        return None


def _containers(base: Path) -> list[Path]:
    try:
        return sorted(p for p in base.iterdir() if p.is_dir())
    except OSError:
        return []


def _container_caches(
    where: str, container: Path, may_open: bool
) -> tuple[list[tuple[str, Path, tuple[Path, ...]]], list[str]]:
    found, skipped = [], []
    for sub in ('Library/Caches/m2v', 'tmp/m2v'):
        parts = sub.split('/')
        way = [container / '/'.join(parts[: i + 1]) for i in range(len(parts) - 1)]
        shut = [d for d in way if os.path.lexists(d) and not readable(d)]
        if not shut:
            if (container / sub).is_dir():
                found.append((f'{where}/{sub}', container / sub, ()))
        elif may_open:
            found.append((f'{where}/{sub}', container / sub, tuple(way)))
        elif sub.startswith('Library'):
            skipped.append(container.name)
    # the plugin's Library/Caches/m2v-<process> of a data container whose tmp is the process's temp directory
    # (66b8ec2: WebKit's GPU process in Safari's container, a poster extension in its own)
    if readable(container / 'Library') and readable(container / 'Library/Caches'):
        found.extend(
            (f'{where}/{m2v.relative_to(container)}', m2v, ())
            for m2v in sorted((container / 'Library/Caches').glob('m2v-*'))
            if m2v.is_dir() and readable(m2v)
        )
    return found, skipped


def merge(corpus: Path, sources: list[tuple[str, str]], build: str) -> list[str]:
    lines, seen = [], set()
    kept = [(corpus, corpus.read_text(errors='replace'), None)] if corpus.is_file() else []
    for name, text, journaled in kept + [(n, t, build) for n, t in sources]:
        added = 0
        for raw in text.splitlines():
            line, key = normal(raw.rstrip('\n'), journaled)
            if line and key not in seen:
                seen.add(key)
                lines.append(line)
                added += 1
        print(f'{name}: {added} new')
    corpus.write_text('\n'.join(lines) + '\n')
    print(
        f'{corpus}: {sum(entry[0] == "S" for entry in lines)} specializations, '
        f'{sum(entry[0] == "R" for entry in lines)} render states, '
        f'{sum(entry[0] == "C" for entry in lines)} compute states, '
        f'{sum(entry[0] == "D" for entry in lines)} dispatch shapes'
    )
    return lines


def collect(m2v: Path, store: Path) -> None:
    """Copy a cache's libraries made from bytes into <store>/data/."""
    for path in sorted((m2v / 'libs').glob('*.metallib')) if (m2v / 'libs').is_dir() else []:
        target = store / 'data' / path.name
        if not target.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)


reply_ok = compiler_replies.reply_ok


def replies(m2v: Path, store: Path) -> int:
    source = m2v / 'msl'
    report = compiler_replies.inventory(source, strict=True)
    copied = compiler_replies.copy_inventory(source, store, report)
    compiler_replies.inventory(store, strict=True)
    return copied


def record_ok(data: bytes) -> bool:
    """Return whether `data` is a whole driver record.

    Magic, identity, a length that ends where the checksum begins, the checksum.
    """
    if len(data) < 8 + 28 + 8 + 32 or data[:8] != RECORD_MAGIC:
        return False
    length = int.from_bytes(data[36:44], 'little')
    return 44 + length + 32 == len(data) and hashlib.sha256(data[:-32]).digest() == data[-32:]


def records(caches: Sequence[str | Path], store: Path) -> None:
    """Copy every driver record of each cache directory in `caches` whose envelope checks out into `store`.

    By name; one already there is kept: a name is what the making reads, so two records of one name hold the same
    driver's data.
    """
    seen = copied = bad = 0
    for cache in caches:
        content = Path(cache) / 'v1' / CONTENT
        for path in sorted(content.glob('*/*')) if content.is_dir() else []:
            if path.name.startswith('.') or not path.is_file():
                continue
            seen += 1
            target = store / path.parent.name / path.name
            if target.is_file():
                continue
            data = path.read_bytes()
            if not record_ok(data):
                bad += 1
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            copied += 1
    in_store = sum(1 for _ in store.glob('*/*')) if store.is_dir() else 0
    print(
        f'{store}: {seen} driver records of {len(caches)} caches, {copied} new, {bad} refused (envelope), '
        f'{in_store} in the store'
    )


def embedded_libraries() -> set[str]:
    """Return the sha256 of each metallib the build makes itself.

    The OS volume's (gpu/apple-libs.sha256) and the frontend's.
    """
    made = {
        line.split()[0]
        for line in (PROJECT / 'gpu/apple-libs.sha256').read_text().splitlines()
        if line.strip() and not line.startswith('#')
    }
    text = (Path(__file__).resolve().parents[2] / 'NativeMetal/TwoPlaneStandIn.h').read_text()
    made.add(
        hashlib.sha256(
            bytes(int(b, 16) for b in re.findall(r'0x([0-9a-fA-F]{2})', text[text.index('{') :]))
        ).hexdigest()
    )
    return made


def pack(store: Path, corpus: Path) -> None:
    """Write the tar of @gpu_device_data (DEVICE_DATA in gpu/deps.bzl) from the staging directory `store`."""
    named = set(re.findall(r'lib=data:([0-9a-f]{64})', corpus.read_text()))
    own = embedded_libraries()
    members = sorted(p for p in (store / 'records').glob('*/*') if p.is_file() and record_ok(p.read_bytes()))
    members += sorted(
        p
        for p in (store / 'data').glob('*.metallib')
        if p.stem in named and p.stem not in own and hashlib.sha256(p.read_bytes()).hexdigest() == p.stem
    )
    reply_inventory = compiler_replies.inventory(store / 'msl', strict=True)
    members += [store / 'msl' / name for name in sorted(reply_inventory['files'])]
    if not members:
        sys.exit(f'{store}: no driver records or libraries staged (harvest.py --volume or --records-from first)')
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for path in members:
            info = tarfile.TarInfo(str(path.relative_to(store)))
            info.size, info.mode, info.mtime = path.stat().st_size, 0o644, 0
            with path.open('rb') as f:
                tar.addfile(info, f)
    digest = hashlib.sha256(data.getvalue()).hexdigest()
    out = DEVICE / f'm2v-device-{digest[:16]}.tar'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data.getvalue())
    record_count = sum(1 for p in members if p.parts[-3] == 'records')
    library_count = sum(1 for p in members if p.parts[-2] == 'data')
    reply_count = reply_inventory['replies']
    print(
        f'{out}: {record_count} driver records, {library_count} libraries '
        f'(of {len(named)} made from bytes the corpus names), {reply_count} compile replies'
    )
    print(f'gpu/deps.bzl:\nDEVICE_DATA = {{\n    "file": "{out.name}",\n    "sha256": "{digest}",\n}}')


def option(args: list[str], name: str, many: bool = False) -> list[str] | str | None:
    values = []
    while name in args:
        i = args.index(name)
        if i + 1 >= len(args):
            sys.exit(__doc__)
        values.append(args.pop(i + 1))
        args.pop(i)
    return values if many else (values[-1] if values else None)


def main() -> None:
    args = sys.argv[1:]
    volume = option(args, '--volume')
    save = option(args, '--save')
    store = Path(option(args, '--device') or DEVICE / 'staging')
    if '--pack' in args:
        pack(store, Path(__file__).resolve().parent / 'corpus.log')
        return
    records_from = option(args, '--records-from', many=True)
    if records_from:
        records_from += args
        records(
            sorted({str(v1.parent) for d in records_from for v1 in Path(d).rglob('v1') if (v1 / CONTENT).is_dir()}),
            store / 'records',
        )
        kept = sum(replies(msl.parent, store / 'msl') for d in records_from for msl in sorted(Path(d).rglob('msl')))
        print(f'{store / "msl"}: {kept} new compile replies')
        return
    explicit = set(option(args, '--open', many=True))
    opens = explicit or set(SYSTEM_CACHES)
    open_all = '--open-all' in args
    args = [a for a in args if a != '--open-all']
    build = option(args, '--build')
    if not args or (volume is None and len(args) < 2) or not build:
        sys.exit(__doc__)
    corpus, sources = Path(args[0]), [(p, Path(p).read_text(errors='replace')) for p in args[1:]]
    if volume:
        volume = Path(volume)
        if not (volume / 'mobile').is_dir():
            sys.exit(f'{volume}: not a mounted iPad Data volume (no mobile/)')
        found, skipped = caches(volume, opens, open_all, print, bool(explicit))
        for where, m2v, through in found:
            with opened((*through, m2v) if through else (), print):
                if not m2v.is_dir():
                    continue
                sources.extend(
                    (f'{where}/{journal}', (m2v / journal).read_text(errors='replace'))
                    for journal in JOURNALS
                    if (m2v / journal).is_file()
                )
                collect(m2v, store)
                records([m2v], store / 'records')
                replies(m2v, store / 'msl')
                if save:
                    target = Path(save) / where.replace('/', '_')
                    shutil.rmtree(target, ignore_errors=True)
                    shutil.copytree(m2v, target, copy_function=shutil.copyfile, ignore=shutil.ignore_patterns('.*.tmp'))
        journals = len(sources) - (len(args) - 1)
        print(
            f'{volume}: {len(found)} m2v caches, {journals} journals; {len(set(skipped))} closed containers not '
            'looked in (--open-all)'
        )
    merge(corpus, sources, build)


if __name__ == '__main__':
    main()
