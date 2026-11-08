#!/usr/bin/env python3
"""Stage the TegraGM20B Metal plugin onto the SD card's iPadOS Data volume, mounted on the Mac.

usage: stage-tegra-gm20b-metal.py <Data mount point> (--build <manifest or dir> | --remove) [--no-raw] [--progress]

--build is //gpu:bundle's manifest (bazel-bin/gpu/bundle.stage, in //deploy:sd's runfiles: tools/gpu/stage_manifest.py),
or a directory laid out as //gpu:base (bazel-bin/gpu/base, an lkg snapshot's gpu/). Exit status 0 on success, else one
error line.

Mount the Data volume read-write first, e.g.
`diskutil mount -mountOptions nobrowse D1B1D053-D0A6-496C-8625-A947A3DA863E` (SwitchRawMount.h SWITCH_RAW_DATA_UUID),
and eject the card before it goes back into the Switch.

What it stages, on the live volume (as vendor/unixnu/scripts/add-gpu-plugin-data.py --publish did on a seed image):
  * <Data>/ngpu/addons/TegraGM20BMetal.bundle. The kernel shows /private/var/ngpu/addons over
    /System/Library/Extensions;
  * <Data>/ngpu/addons/TegraGM20BMetal.m2v-seed beside it, when the build has one (the shader cache seed);
  * <Data>/ngpu/addons/TegraGM20BMetal.raw beside it, when the build has one (the raw GM20B path's pipeline pack: with
    it staged, backboardd draws through the raw path; --no-raw, or TEGRA_RAW_STAGE=0 in the environment, leaves it
    out, and the render server keeps the m2v device);
  * CAEnableDeepFramebuffer, CAEnableFramebufferCompression and CAEnableIOSurfaceCompression = false merged into the
    preferences backboardd reads when it starts, before any plugin code runs (its own domain and the global one, for
    mobile and root): the GPU and the display driver take plain 8-bit rows.
Only files whose sha256 differs from what the card holds are copied, and files the build no longer has are removed:
<Data>/ngpu/stage.tsv.gz lists what is staged (its first line the whole list's digest, so an unchanged build reads
nothing else), and <Data>/ngpu/stage.pending the files a stage was writing, which the next one copies again if it was
cut short. A card without the list has the three directories replaced whole.
--remove deletes the three directories and the list (the preferences stay; without a Metal device they change nothing).
--progress prints 'progress DONE/TOTAL bytes' as the files land and 'progress WHAT' before a long wait, for the deploy
and the image builder to show.
The CDHashes must also be in the boot entry's static trust cache (//boot:package appends //gpu:trustcache, the same
TegraGM20BMetal.trustcache), so stage and package from the same build.
"""

import gzip
import hashlib
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
SEED = ADDONS + '/TegraGM20BMetal.m2v-seed'
RAW = ADDONS + '/TegraGM20BMetal.raw'
OWNED = (BUNDLE, SEED, RAW)
STATE = 'ngpu/stage.tsv.gz'
PENDING = 'ngpu/stage.pending'
MANIFEST_HEADER = '# gpu stage 1 '
EXECUTABLES = ('TegraGM20BMetal', 'libvulkan_nouveau.dylib')
PREFS = {
    'CAEnableDeepFramebuffer': False,
    'CAEnableFramebufferCompression': False,
    'CAEnableIOSurfaceCompression': False,
}

# path below the volume -> (source file, sha256, size, mode)
Wanted = dict[str, tuple[Path, str, int, int]]


def _arguments(argv: list[str]) -> tuple[Path, Path | None, bool, bool, bool]:
    remove = '--remove' in argv
    raw = '--no-raw' not in argv and os.environ.get('TEGRA_RAW_STAGE', '1') != '0'
    progress = '--progress' in argv
    argv = [a for a in argv if a not in ('--no-raw', '--progress')]
    build = None
    if '--build' in argv[:-1]:
        build = Path(os.path.abspath(argv[argv.index('--build') + 1]))
        del argv[argv.index('--build') : argv.index('--build') + 2]
    args = [a for a in argv if a != '--remove']
    if len(args) != 1 or (build is None and not remove):
        sys.exit(__doc__)
    return Path(args[0]).resolve(), build, remove, raw, progress


def _check_volume(data: Path) -> None:
    info = plistlib.loads(
        subprocess.run(['diskutil', 'info', '-plist', str(data)], capture_output=True, check=True).stdout
    )
    if info.get('VolumeUUID') != DATA_UUID or info.get('MountPoint') != str(data) or not info.get('WritableVolume'):
        sys.exit(f'{data} is not the iPadOS Data volume {DATA_UUID} mounted read-write')
    if not (data / 'mobile').is_dir():
        sys.exit(f'{data} has no mobile/; wrong volume?')


def from_manifest(manifest: Path) -> Wanted:
    """Read //gpu:bundle's manifest; its sources are below the runfiles root its own rlocation path names."""
    lines = manifest.read_text().splitlines()
    if not lines or not lines[0].startswith(MANIFEST_HEADER):
        sys.exit(f'stage-tegra-gm20b-metal: {manifest} is not a GPU stage manifest (bazel build //gpu:bundle)')
    own = lines[0][len(MANIFEST_HEADER) :]
    if not str(manifest).endswith('/' + own):
        sys.exit(f'stage-tegra-gm20b-metal: {manifest} is not in a runfiles tree (run it as bazel run //deploy:sd)')
    root = Path(str(manifest)[: -len(own)])
    wanted = {}
    for line in lines[1:]:
        path, digest, size, mode, source = line.split('\t')
        wanted[path] = (root / source, digest, int(size), int(mode, 8))
    return wanted


def from_directory(build: Path) -> Wanted:
    """Hash a directory laid out as //gpu:base (an lkg snapshot's gpu/, or the image builder's)."""
    if not (build / 'TegraGM20BMetal.trustcache').is_file():
        sys.exit(f'stage-tegra-gm20b-metal: no plugin build in {build} (bazel build //gpu:bundle)')
    wanted = {}
    for top in OWNED:
        for path in sorted(p for p in (build / top).rglob('*') if p.is_file()):
            data = path.read_bytes()
            mode = 0o755 if path.name in EXECUTABLES else 0o644
            wanted[path.relative_to(build).as_posix()] = (path, hashlib.sha256(data).hexdigest(), len(data), mode)
    return wanted


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


def sync(data: Path, wanted: Wanted, report: Callable[[str], None]) -> str:
    """Make the card hold `wanted` under the three staged directories; return what was done."""
    for directory in (data / 'ngpu', data / ADDONS):
        directory.mkdir(exist_ok=True)
        directory.chmod(0o755)
    whole = digest(wanted)
    known = read_state(data, whole)
    if known is True:
        return f'unchanged ({len(wanted)} files)'
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
    copied = sum(wanted[p][2] for p in changed)
    return f'{len(changed)} copied ({copied >> 20} MiB), {len(stale)} removed, {len(wanted) - len(changed)} unchanged'


def _merge_preferences(data: Path) -> None:
    for home, owner in (('mobile', 501), ('root', 0)):
        directory = data / home / 'Library/Preferences'
        directory.mkdir(parents=True, exist_ok=True)
        # backboardd's defaults domain is its bundle ID, com.apple.backboardd; the global domain is what took effect
        # so far
        for name in ('com.apple.backboardd.plist', 'backboardd.plist', '.GlobalPreferences.plist'):
            file = directory / name
            values = plistlib.loads(file.read_bytes()) if file.exists() else {}
            if all(values.get(k) == v for k, v in PREFS.items()):
                continue
            values.update(PREFS)
            file.write_bytes(plistlib.dumps(values, fmt=plistlib.FMT_BINARY))
            file.chmod(0o644)
            if os.geteuid() == 0:
                os.chown(file, owner, owner)


def _silent(_: str) -> None:
    pass


def _report(what: str) -> None:
    print('progress', what, flush=True)


def main() -> None:
    data, build, remove, raw, progress = _arguments(sys.argv[1:])
    report = _report if progress else _silent
    _check_volume(data)
    if remove:
        for top in OWNED:
            shutil.rmtree(data / top, ignore_errors=True)
        for name in (STATE, PENDING):
            (data / name).unlink(missing_ok=True)
        print('removed', data / BUNDLE)
        return
    if not build.is_file():
        report('hashing the build')
    wanted = from_manifest(build) if build.is_file() else from_directory(build)
    if BUNDLE + '/TegraGM20BMetal' not in wanted:
        sys.exit(f'stage-tegra-gm20b-metal: no plugin build in {build} (bazel build //gpu:bundle)')
    if not raw:
        wanted = {p: w for p, w in wanted.items() if not p.startswith(RAW + '/')}
    done = sync(data, wanted, report)
    _merge_preferences(data)
    report('flushing to the card')
    subprocess.run(['sync'], check=True)
    staged = sorted({p.split('/')[2] for p in wanted})
    print('staged', data / BUNDLE, staged, done)


if __name__ == '__main__':
    main()
