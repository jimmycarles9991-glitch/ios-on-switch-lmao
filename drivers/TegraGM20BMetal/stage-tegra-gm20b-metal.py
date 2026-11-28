#!/usr/bin/env python3
r"""Stage the TegraGM20B Metal plugin onto the SD card's iPadOS Data volume, mounted on the Mac.

usage: stage-tegra-gm20b-metal.py <Data mount point> (--build <manifest or dir> | --remove) [--progress]

--build is a GPU stage manifest (tools/gpu/stage_format.py): //gpu:bundle's (bazel-bin/gpu/bundle.stage, in
//deploy:sd's runfiles), or a stage tree's gpu.stage. A directory stands for the gpu.stage at its top: a stage tree
(//gpu:stage_tree), //gpu:base or an lkg snapshot's gpu/. Exit status 0 on success, else one error line.

Mount the Data volume read-write first, e.g.
`diskutil mount -mountOptions nobrowse D1B1D053-D0A6-496C-8625-A947A3DA863E` (SwitchRawMount.h SWITCH_RAW_DATA_UUID),
and eject the card before it goes back into the Switch.

What it stages, on the live volume (as vendor/unixnu/scripts/add-gpu-plugin-data.py --publish did on a seed image):
  * <Data>/ngpu/addons/TegraGM20BMetal.bundle. The kernel shows /private/var/ngpu/addons over
    /System/Library/Extensions;
  * <Data>/ngpu/addons/TegraGM20BMetal.seed beside it, when the build has one (the Metal compiler's replies the
    shader compiler reads);
  * <Data>/ngpu/addons/TegraGM20BMetal.pack beside it (the driver's pipeline pack: a process that cannot read
    it gets no Metal device);
  * the manifest's preference records merged into the plists they name (backboardd's Core Animation preferences,
    which it reads when it starts, before any plugin code runs).
Only files whose sha256 differs from what the card holds are copied, and files the build no longer has are removed. A
card without the list below has the three directories replaced whole.

What the card holds, which an incremental stage reads back first:
  <Data>/ngpu/stage.tsv.gz  gzip text. Line 1 is '# <digest>': the sha256 of the lines that follow, which are one
                            '<path>\t<sha256>\t<size>\t<mode, octal>' per staged file, in path order, each followed
                            by a newline. A matching digest with no stage.pending means nothing changed, and nothing
                            else is read.
  <Data>/ngpu/stage.pending one path a line: what a stage was copying or removing when it began. The next stage takes
                            those as not staged, so a cut-short stage copies them again; removed when a stage ends.
Preferences are not in the list: they are merged on every stage.
<Data>/Switch/icon-cache-gpu names the staged build (the list's digest with the manifest's `# kext` identity of the
kernel's GPU kext folded in, empty once removed), on every path that stages. The kext identity covers the three
TegraGM20B libraries only: a change to IOSurface, the SMMU, host1x or display flip code keeps the name.
iconservicesagent keeps the icons it drew while __cache_info__ names the system's ProductBuildVersion, which a GPU build
does not change, so the next boot's kernel lets them go once per build it names (kernel/switch/boot/icon_cache.c).
--remove deletes the three directories and the list (the preferences stay; without a Metal device they change nothing).
--progress prints 'progress DONE/TOTAL bytes' as the files land and 'progress WHAT' before a long wait, for the deploy
and the image builder to show.
The CDHashes must also be in the boot entry's static trust cache (//boot:package appends //gpu:trustcache, the same
TegraGM20BMetal.trustcache), so stage and package from the same build.
"""

import gzip
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

DATA_UUID = 'D1B1D053-D0A6-496C-8625-A947A3DA863E'
ADDONS = 'ngpu/addons'
BUNDLE = ADDONS + '/TegraGM20BMetal.bundle'
SEED = ADDONS + '/TegraGM20BMetal.seed'
RAW = ADDONS + '/TegraGM20BMetal.pack'
OWNED = (BUNDLE, SEED, RAW)
# What builds before the driver left raw/ staged in their place: a card staged then still holds them
RENAMED_FROM = (ADDONS + '/TegraGM20BMetal.m2v-seed', ADDONS + '/TegraGM20BMetal.raw')
STATE = 'ngpu/stage.tsv.gz'
PENDING = 'ngpu/stage.pending'
ICON_CACHE_RECORD = 'Switch/icon-cache-gpu'
MANIFEST_HEADER = '# gpu stage 2 '
MANIFEST_KEXT = '# kext '
TREE_MANIFEST = 'gpu.stage'

# path below the volume -> (source file, sha256, size, mode)
Wanted = dict[str, tuple[Path, str, int, int]]
# plist below the volume -> (owner uid, key -> value)
Prefs = dict[str, tuple[int, dict]]


def _arguments(argv: list[str]) -> tuple[Path, Path | None, bool, bool]:
    remove = '--remove' in argv
    progress = '--progress' in argv
    argv = [a for a in argv if a != '--progress']
    build = None
    if '--build' in argv[:-1]:
        build = Path(os.path.abspath(argv[argv.index('--build') + 1]))
        del argv[argv.index('--build') : argv.index('--build') + 2]
    args = [a for a in argv if a != '--remove']
    if len(args) != 1 or (build is None and not remove):
        sys.exit(__doc__)
    return Path(args[0]).resolve(), build, remove, progress


def _check_volume(data: Path) -> None:
    info = plistlib.loads(
        subprocess.run(['/usr/sbin/diskutil', 'info', '-plist', str(data)], capture_output=True, check=True).stdout
    )
    if info.get('VolumeUUID') != DATA_UUID or info.get('MountPoint') != str(data) or not info.get('WritableVolume'):
        sys.exit(f'{data} is not the iPadOS Data volume {DATA_UUID} mounted read-write')
    if not (data / 'mobile').is_dir():
        sys.exit(f'{data} has no mobile/; wrong volume?')


def _below(path: str) -> str:
    parts = Path(path).parts
    if not parts or path.startswith('/') or '..' in parts:
        sys.exit(f'stage-tegra-gm20b-metal: {path!r} is not a path below the Data volume')
    return path


def from_manifest(manifest: Path) -> tuple[Wanted, Prefs]:
    """Read a GPU stage manifest (tools/gpu/stage_format.py): its files and its preferences."""
    lines = manifest.read_text().splitlines()
    if not lines or not lines[0].startswith(MANIFEST_HEADER):
        sys.exit(
            f'stage-tegra-gm20b-metal: {manifest} is not a GPU stage manifest, format 2 (bazel build //gpu:bundle)'
        )
    form, _, own = lines[0][len(MANIFEST_HEADER) :].partition(' ')
    if form == 'tree' and not own:
        root = manifest.parent
    elif form == 'runfiles' and own and str(manifest).endswith('/' + own):
        root = Path(str(manifest)[: -len(own)])
    else:
        sys.exit(f'stage-tegra-gm20b-metal: {manifest} is not in a runfiles tree (run it as bazel run //deploy:sd)')
    wanted, prefs = {}, {}
    for line in lines[1:]:
        if line.startswith(MANIFEST_KEXT):
            continue
        kind, *fields = line.split('\t')
        if kind == 'file':
            path, sha, size, mode, source = fields
            wanted[_below(path)] = (root / _below(source), sha, int(size), int(mode, 8))
        elif kind == 'pref':
            plist, uid, key, value = fields
            plist_path, owner = _below(plist), int(uid)
            existing_owner, values = prefs.setdefault(plist_path, (owner, {}))
            if existing_owner != owner:
                sys.exit(f'stage-tegra-gm20b-metal: {manifest}: conflicting owners for {plist_path}')
            values[key] = json.loads(value)
        else:
            sys.exit(f'stage-tegra-gm20b-metal: {manifest}: no record kind {kind!r}')
    return wanted, prefs


def from_directory(build: Path) -> tuple[Wanted, Prefs]:
    """Read the manifest at the top of a stage tree, //gpu:base or an lkg snapshot's gpu/."""
    if not (build / TREE_MANIFEST).is_file():
        sys.exit(f'stage-tegra-gm20b-metal: {build} has no {TREE_MANIFEST} (built before gpu stage 2: build it again)')
    return from_manifest(build / TREE_MANIFEST)


def digest(wanted: dict) -> str:
    """Return one sha256 over what is staged: every path with its file's sha256, size and mode."""
    whole = hashlib.sha256()
    for path in sorted(wanted):
        _, sha, size, mode = wanted[path]
        whole.update(f'{path}\t{sha}\t{size}\t{mode:o}\n'.encode())
    return whole.hexdigest()


def read_state(data: Path, whole: str) -> bool | dict[str, tuple[str, int, int]] | None:
    """Return what the card's list says is staged, True when it matches `whole`, None without a list."""
    state = data / STATE
    if not state.is_file():
        return None
    with gzip.open(state, 'rt') as f:
        if f.readline().strip() == '# ' + whole and not (data / PENDING).exists():
            return True
        known = {}
        for line in f:
            path, sha, size, mode = line.rstrip('\n').split('\t')
            known[path] = (sha, int(size), int(mode, 8))
    pending = data / PENDING
    for path in pending.read_text().splitlines() if pending.is_file() else []:
        known.pop(path, None)
    return known


def _write_atomically(path: Path, write: Callable[[Path], object]) -> None:
    partial = path.with_name(path.name + '.partial')
    write(partial)
    partial.replace(path)


def _copy(source: Path, target: Path, mode: int) -> None:
    shutil.copyfile(source, target)
    target.chmod(mode)


def _remove(data: Path, paths: list[str]) -> None:
    """Remove the files at `paths`, and every directory below ngpu/addons that leaves empty."""
    for path in paths:
        (data / path).unlink(missing_ok=True)
    for parent in sorted({(data / p).parent for p in paths}, key=lambda d: -len(d.parts)):
        empty = parent
        while empty != data / ADDONS and empty.is_dir() and not any(empty.iterdir()):
            empty.rmdir()
            empty = empty.parent


def _remove_renamed(data: Path) -> list[str]:
    """Remove the folders RENAMED_FROM names; return those there were."""
    removed = [old for old in RENAMED_FROM if (data / old).is_dir()]
    for old in removed:
        shutil.rmtree(data / old)
    return removed


def _copy_all(data: Path, wanted: Wanted, changed: list[str], report: Callable[[str], None]) -> None:
    total, done = sum(wanted[p][2] for p in changed), 0
    report(f'{done}/{total} bytes')
    with ThreadPoolExecutor(8) as pool:
        copies = {pool.submit(_copy, wanted[p][0], data / p, wanted[p][3]): p for p in changed}
        try:
            for copied in as_completed(copies):
                copied.result()
                done += wanted[copies[copied]][2]
                report(f'{done}/{total} bytes')
        finally:
            for pending in copies:
                pending.cancel()


def _data_folder(data: Path) -> Path:
    """Return the Data volume's own folder: Switch, or the former one while only it exists (tools/install/renamed.py)."""
    folder = data / ICON_CACHE_RECORD.split('/', 1)[0]
    if folder.exists():
        return folder

    def ours(folder: Path) -> bool:
        try:
            return folder.is_dir() and not folder.is_symlink() and (folder / 'overlay').is_dir()
        except PermissionError:  # root's 0700 folders, closed to the Mac's user: not one of ours
            return False

    older = [p for p in data.iterdir() if ours(p)]
    return older[0] if len(older) == 1 else folder


def manifest_kext(manifest: Path) -> str:
    """Return the GPU kext's build identity a GPU stage manifest names, '' where it names none."""
    path = manifest / TREE_MANIFEST if manifest.is_dir() else manifest
    for line in path.read_text().splitlines()[1:3]:
        if line.startswith(MANIFEST_KEXT):
            return line[len(MANIFEST_KEXT) :]
    return ''


def icon_build(whole: str, kext: str) -> str:
    """Name what draws icons: the staged list and, where known, the kernel's GPU kext, which draws through them too."""
    return hashlib.sha256(f'{whole}\n{kext}'.encode()).hexdigest() if kext else whole


def name_for_icon_cache(data: Path, build: str) -> None:
    """Name the staged build in ICON_CACHE_RECORD, writing nothing when it already does."""
    record = _data_folder(data) / ICON_CACHE_RECORD.split('/', 1)[1]
    if record.is_file() and record.read_text() == build + '\n':
        return
    record.parent.mkdir(parents=True, exist_ok=True)
    _write_atomically(record, lambda f: f.write_text(build + '\n'))


def sync(data: Path, wanted: Wanted, report: Callable[[str], None], kext: str = '') -> str:
    """Make the card hold `wanted` under the three staged directories; return what was done."""
    for directory in (data / 'ngpu', data / ADDONS):
        directory.mkdir(exist_ok=True)
        directory.chmod(0o755)
    whole = digest(wanted)
    known = read_state(data, whole)
    renamed = ''.join(f'; removed {old}, which the rename replaced' for old in _remove_renamed(data))
    if known is True:
        name_for_icon_cache(data, icon_build(whole, kext))
        return f'unchanged ({len(wanted)} files){renamed}'
    if known is None:
        for top in OWNED:
            shutil.rmtree(data / top, ignore_errors=True)
        known = {}
    changed = [p for p, (_, sha, size, mode) in wanted.items() if known.get(p) != (sha, size, mode)]
    stale = [p for p in known if p not in wanted]
    _write_atomically(data / PENDING, lambda f: f.write_text(''.join(p + '\n' for p in changed + stale)))
    _remove(data, stale)
    for directory in sorted({(data / p).parent for p in changed}):
        directory.mkdir(mode=0o755, parents=True, exist_ok=True)
    _copy_all(data, wanted, changed, report)

    def write_state(path: Path) -> None:
        with gzip.open(path, 'wt', compresslevel=1) as f:
            f.write(f'# {whole}\n')
            for p in sorted(wanted):
                _, sha, size, mode = wanted[p]
                f.write(f'{p}\t{sha}\t{size}\t{mode:o}\n')

    _write_atomically(data / STATE, write_state)
    (data / PENDING).unlink()
    name_for_icon_cache(data, icon_build(whole, kext))
    copied = sum(wanted[p][2] for p in changed)
    return (
        f'{len(changed)} copied ({copied >> 20} MiB), {len(stale)} removed, {len(wanted) - len(changed)} unchanged'
        + renamed
    )


def _merge_preferences(data: Path, prefs: Prefs) -> None:
    for plist, (owner, values) in sorted(prefs.items()):
        file = data / plist
        file.parent.mkdir(parents=True, exist_ok=True)
        merged = plistlib.loads(file.read_bytes()) if file.exists() else {}
        if all(k in merged and type(merged[k]) is type(v) and merged[k] == v for k, v in values.items()):
            continue
        merged.update(values)
        file.write_bytes(plistlib.dumps(merged, fmt=plistlib.FMT_BINARY))
        file.chmod(0o644)
        if os.geteuid() == 0:
            os.chown(file, owner, owner)


def _silent(_: str) -> None:
    pass


def _report(what: str) -> None:
    print('progress', what, flush=True)


def main() -> None:
    data, build, remove, progress = _arguments(sys.argv[1:])
    report = _report if progress else _silent
    _check_volume(data)
    if remove:
        for top in OWNED:
            shutil.rmtree(data / top, ignore_errors=True)
        for old in _remove_renamed(data):
            print('removed', data / old)
        for name in (STATE, PENDING):
            (data / name).unlink(missing_ok=True)
        name_for_icon_cache(data, '')
        print('removed', data / BUNDLE)
        return
    wanted, prefs = from_manifest(build) if build.is_file() else from_directory(build)
    if BUNDLE + '/TegraGM20BMetal' not in wanted:
        sys.exit(f'stage-tegra-gm20b-metal: no plugin build in {build} (bazel build //gpu:bundle)')
    done = sync(data, wanted, report, manifest_kext(build))
    _merge_preferences(data, prefs)
    report('flushing to the card')
    subprocess.run(['/bin/sync'], check=True)
    staged = sorted({p.split('/')[2] for p in wanted})
    print('staged', data / BUNDLE, staged, done)


if __name__ == '__main__':
    main()
