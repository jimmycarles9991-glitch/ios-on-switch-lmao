#!/usr/bin/env python3
"""Pull the last boots' logs off the card's iPad Data volume (read-only but for harvest.py's one documented write).

    bazel run //deploy:pull_logs -- <diskN> [--out DIR] [--no-logarchive] [--harvest] [--no-csstore] [--no-launchd]
                                            [--no-eject]

The unified log (Data db/diagnostics and db/uuidtext) is mirrored in <state>/device-logs/mirror, copying only the files
that are new or changed since the last pull (a closed tracev3 chunk never changes). Into <out> (default
<state>/device-logs/boot-<MMDD-HHMM>, <state> being ~/Library/Application Support/switch-ios or $SWITCH_HOME: outside
every checkout) it copies:
  boot.logarchive                       the mirror as a logarchive, its files hard links into the mirror (`/usr/bin/log
                                        show --archive <out>/boot.logarchive --style compact --info --debug`)
  launchd.log                           (--no-launchd skips it)
  mobile-CrashReporter/, *.ips          crash reports, the WebKit ones also at the top level
  mobileactivationd/, migration-state.txt, boot-io-census.bin
  panic-<UTC>.log, .kcdata              the last saved kernel panic (SwitchPanicSave.c: the panic string, kernel log
                                        tail and XNU's panic log; its stackshot), with the stackshot decoded by XNU's
                                        kcdata.py into panic-<UTC>.stackshot.ips (every task's threads, as stacks-*.ips)
                                        and panic-<UTC>.threads.txt (the threads of the process the panic names, e.g.
                                        the one a "userspace watchdog timeout" found hung)
  power.log, power.prev.log            the power journal (SwitchWake.h: every display off/on, wake source, wake
                                        guarantee and system sleep step) of the last boot and the one before
  wake-<UTC>.kcdata                     a stackshot of every task taken when the wake guarantee lit the panel itself (a
                                        wake source with the panel off and no display-on from the render server),
                                        decoded as the panic ones into wake-<UTC>.stackshot.ips and
                                        wake-<UTC>.threads.txt (SpringBoard's and backboardd's threads)
  com.apple.LaunchServices-*.csstore + lsdb-webkit.txt   (--no-csstore skips them)
  <boot>-<stamp>.json, hwtest-summary.txt  HW Test's run reports (also kept in <state>/device-logs/hwtest) and the
                                        newest new run of each app against the runs before it (tools/hwtest)
  hwtest-gpu/<bundle id>/               HW Test's Documents/gpu: the GPU scenes' got, want and diff PNGs
  hwtest-ui/<bundle id>/                HW Test's Documents/ui: the UI tests' failed pictures (display.pocket_strip's)
  lock-poster/                          the lock-screen poster's newest landscape render and its portrait one, as PNGs
                                        (tools/hwtest/posters.py; its verdict is in the log and hwtest-summary.txt)
  screenshots/                          the screenshots the user took on the device (Photos' DCIM)
  m2v/ (+ m2v-cache -> backboardd's)    with --harvest (or --//deploy:harvest): every process's m2v shader cache,
                                        whose journals are merged into the precompile corpus
                                        (drivers/TegraGM20BMetal/precompile/corpus.log, a source file in your checkout)
                                        by that directory's harvest.py
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.common import files, paths
from tools.deploy import app_journal, deployed, mkarchive
from tools.hwtest import hwtest
from tools.install import (
    card as cards,
    host,
    renamed,
)

DATA_UUID = 'D1B1D053-D0A6-496C-8625-A947A3DA863E'  # SwitchRawMount.h SWITCH_RAW_DATA_UUID (checked against spec)
WEBKIT_IDS = (
    'com.apple.WebKit.WebContent',
    'com.apple.WebKit.Networking',
    'com.apple.WebKit.GPU',
    'com.apple.WebKit.WebContent.CaptivePortal',
    'WebContentExtension.appex',
    'com.apple.fskit.exfat',
)
BACKBOARDD_CACHE = 'm2v/System_6C7A65AF-CB18-421C-A07A-D724B05A7AB6_Library_Caches_m2v'


def copytree(src: Path, dst: Path, log: host.Log) -> bool:
    if src.is_dir():
        shutil.copytree(src, dst, copy_function=shutil.copyfile, dirs_exist_ok=True)
        log.quiet('copied', src, '->', dst)
        return True
    return False


def mirror(src: Path, dst: Path) -> tuple[int, int]:
    """Make `dst` hold `src`'s files, copying only those it lacks or holds at another size or time.

    Returns the files and bytes copied. A changed file is written beside and renamed over the old one, so the
    logarchives linked to the old one keep it.
    """
    wanted, copied, size = set(), 0, 0
    for path in sorted(src.rglob('*')) if src.is_dir() else []:
        if not path.is_file():
            continue
        relative = path.relative_to(src)
        wanted.add(relative)
        stat, target = path.stat(), dst / relative
        held = target.stat() if target.is_file() else None
        if held and held.st_size == stat.st_size and int(held.st_mtime) == int(stat.st_mtime):
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        files.copy(path, target, keep_times=True)
        copied, size = copied + 1, size + stat.st_size
    for path in sorted(dst.rglob('*')) if dst.is_dir() else []:
        if path.is_file() and path.relative_to(dst) not in wanted:
            path.unlink()
    return copied, size


def report_body(text: str) -> dict:
    """Return the JSON body of an .ips report (its first line is the header)."""
    return json.loads(text.split('\n', 1)[1])


# "userspace watchdog timeout: no successful checkins from com.apple.backboardd (2 induced crashes) ...", the kernel's
# "watchdog timeout: no checkins from watchdogd ..." and launchd's critical exits ("backboardd[53] exited -- ...").
PANIC_NAMES = (
    r'no successful checkins from (?:com\.apple\.)?([\w.-]+)',
    r'no checkins from ([\w.-]+)',
    r'^panic: +([\w.-]+)\[\d+\] exited',
)


def panic_process(panic_text: str) -> str | None:
    """Return the process a panic names (lower case), or None."""
    for pattern in PANIC_NAMES:
        match = re.search(pattern, panic_text, re.MULTILINE)
        if match:
            return match.group(1).lower()
    return None


def thread_summary(report: dict, process: str) -> list[str]:
    """Summarize the threads of every task named `process` (case-insensitive) in a stackshot report.

    Each thread's state, wait and frames as image+offset (S: the dyld shared cache, slid base in binaryImages; K/U: the
    kernel).
    """
    images = report.get('binaryImages', [])

    def frame(entry: list[int]) -> str:
        index, offset = entry
        uuid, _base, kind = images[index] if 0 <= index < len(images) else ('?', 0, '?')
        return f'{kind}:{uuid[:8]}+0x{offset:x}'

    lines = []
    for pid, task in sorted(report.get('processByPid', {}).items(), key=lambda item: int(item[0])):
        if (task.get('procname') or '').lower() != process:
            continue
        threads = task.get('threadById', {})
        lines.append('{}[{}]: {} threads'.format(task.get('procname'), pid, len(threads)))
        lines.extend('  wait: ' + wait for wait in task.get('waitInfo', []))
        for tid, thread in sorted(threads.items(), key=lambda item: int(item[0])):
            name = thread.get('name') or thread.get('dispatch_queue_label') or ''
            lines.append('  thread {} {} {}'.format(tid, name, ','.join(thread.get('state', []))))
            for kind, key in (('kernel', 'kernelFrames'), ('user', 'userFrames')):
                frames = thread.get(key) or []
                if frames:
                    lines.append('    {}: {}'.format(kind, ' '.join(frame(f) for f in frames)))
    return lines


def decode_stackshot(shot: Path, kcdata: str | Path | None, log: host.Log, what: str) -> Path | None:
    """<shot>.kcdata -> <stem>.stackshot.ips (kcdata.py -s); the .ips path, or None (logged)."""
    stem = shot.with_suffix('')
    if not kcdata or not Path(kcdata).is_file():
        log(what, 'stackshot', shot.name, 'NOT decoded: no kcdata.py (run as bazel run //deploy:pull_logs)')
        return None
    ips = stem.with_name(stem.name + '.stackshot.ips')
    result = subprocess.run(
        [sys.executable, '-I', str(kcdata), '-s', str(ips), str(shot)], capture_output=True, text=True, check=False
    )
    if result.returncode or not ips.is_file():
        log(what, 'stackshot', shot.name, 'NOT decoded:', (result.stderr or result.stdout).strip()[-300:])
        return None
    log(what, 'stackshot ->', ips)
    return ips


def decode_wakes(out: Path, kcdata: str | Path | None, log: host.Log) -> None:
    """Decode wake-*.kcdata (the wake guarantee's stackshots) -> .stackshot.ips and .threads.txt.

    The threads are SpringBoard's and backboardd's, the two that should have turned the panel on.
    """
    for shot in sorted(Path(out).glob('wake-*.kcdata')):
        ips = decode_stackshot(shot, kcdata, log, 'wake')
        if not ips:
            continue
        body = report_body(ips.read_text())
        lines = []
        for process in ('springboard', 'backboardd'):
            lines += thread_summary(body, process)
        if lines:
            stem = shot.with_suffix('')
            summary = stem.with_name(stem.name + '.threads.txt')
            summary.write_text(
                "# SpringBoard's and backboardd's threads in the wake stackshot {} (S: dyld shared cache "
                'offsets)\n{}\n'.format(ips.name, '\n'.join(lines))
            )
            log('wake stackshot threads ->', summary)


def decode_panics(out: Path, kcdata: str | Path | None, log: host.Log) -> None:
    """panic-*.kcdata -> .stackshot.ips (kcdata.py -s) and .threads.txt for the process its panic string names."""
    for shot in sorted(Path(out).glob('panic-*.kcdata')):
        stem = shot.with_suffix('')
        ips = decode_stackshot(shot, kcdata, log, 'panic')
        if not ips:
            if not kcdata or not Path(kcdata).is_file():
                return
            continue
        text = stem.with_suffix('.log')
        process = panic_process(text.read_text(errors='replace')) if text.is_file() else None
        if not process:
            continue
        lines = thread_summary(report_body(ips.read_text()), process)
        if lines:
            summary = stem.with_name(stem.name + '.threads.txt')
            summary.write_text(
                "# {}'s threads in the panic stackshot {} (S: dyld shared cache offsets)\n{}\n".format(
                    process, ips.name, '\n'.join(lines)
                )
            )
            log('hung process', process, '->', summary)


def deployed_build(data: Path) -> str:
    """Give the IPSW build the last deploy installed on the card (deployed.json), '' where it names none."""
    try:
        return json.loads((renamed.data_folder(data) / deployed.NAME).read_text()).get('build', '')
    except (OSError, ValueError):
        return ''


def harvest(data: Path, out: Path, log: host.Log, harvest_dir: str | None, corpus: str | None = None) -> None:
    """Merge the m2v journals of every process's cache into the checkout's corpus.log (harvest.py)."""
    # harvest_dir: the checkout's precompile directory (harvest.py and the corpus.log it merges into), relative to the
    # workspace: --harvest-script's parent (//deploy's $(rootpath) of harvest.py, so it follows the file if it moves).
    script = paths.workspace() / harvest_dir / 'harvest.py' if harvest_dir else None
    if not (script and script.is_file()):
        log('m2v harvest skipped: no', script)
        return
    corpus_file = Path(corpus or script.parent / 'corpus.log')
    build = deployed_build(data)
    if not build:
        log(f"m2v harvest skipped: {deployed.NAME} names no OS build, so the journals' lines would name none")
        return
    out.mkdir(parents=True, exist_ok=True)
    save = ['--save', str(out / 'm2v')]
    result = subprocess.run(
        [sys.executable, str(script), str(corpus_file), '--build', build, '--volume', str(data), *save],
        capture_output=True,
        text=True,
        check=False,
    )
    log.quiet(result.stdout + result.stderr)
    log('m2v journals {} into {}'.format('harvested' if result.returncode == 0 else 'NOT harvested', corpus_file))
    if (out / BACKBOARDD_CACHE).is_dir():
        link = out / 'm2v-cache'
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(BACKBOARDD_CACHE)


@dataclass(frozen=True)
class Pull:
    """What a pull copies beyond the unified log and the device's own saved files."""

    logarchive: bool = True
    csstore: bool = True
    launchd: bool = True
    kcdata: str | Path | None = None


def _reports(data: Path, out: Path, log: host.Log, launchd: bool) -> None:
    """Copy launchd's log, the crash reports (the WebKit ones also at the top level) and mobileactivationd's logs."""
    if launchd and (data / 'log/com.apple.xpc.launchd/launchd.log').is_file():
        shutil.copyfile(data / 'log/com.apple.xpc.launchd/launchd.log', out / 'launchd.log')
    crash = data / 'mobile/Library/Logs/CrashReporter'
    copytree(crash, out / 'mobile-CrashReporter', log)
    for report in sorted(crash.glob('com.apple.WebKit.*.ips')) if crash.is_dir() else []:
        shutil.copyfile(report, out / report.name)
    copytree(data / 'mobile/Library/Logs/mobileactivationd', out / 'mobileactivationd', log)


def _saved(data: Path, out: Path, log: host.Log, kcdata: str | Path | None) -> None:
    """Copy what the kernel saved in the Data volume's own folder: the I/O census, panics, power journals, wakes."""
    own = renamed.data_folder(data)
    if not own.is_dir():
        return
    census = own / 'boot-io-census.bin'
    if census.is_file():
        shutil.copyfile(census, out / census.name)
    for pattern, what, decode in (
        ('panic-*', 'saved panics:', decode_panics),
        ('power*.log', 'power journal:', None),
        ('wake-*.kcdata', 'wake guarantee stackshots:', decode_wakes),
    ):
        found = sorted(own.glob(pattern))
        for path in found:
            shutil.copyfile(path, out / path.name)
        if found:
            log(what, ', '.join(path.name for path in found))
            if decode:
                decode(out, kcdata, log)


def _screenshots(data: Path, out: Path, log: host.Log) -> None:
    """Copy the screenshots in Photos' DCIM: the one view of what the panel showed that the device keeps."""
    shots = sorted((data / 'mobile/Media/DCIM').glob('*/IMG_*.PNG'))
    if not shots:
        return
    (out / 'screenshots').mkdir(exist_ok=True)
    for shot in shots:
        shutil.copyfile(shot, out / 'screenshots' / shot.name)
    log('screenshots:', ', '.join(shot.name for shot in shots))


def _migration_state(data: Path, out: Path) -> None:
    lines = []
    for name in ('staged_system_apps', 'datamigrator', 'Switch'):
        path = data / name
        lines.append('{}: {}'.format(name, sorted(p.name for p in path.iterdir()) if path.is_dir() else 'missing'))
    apps = data / 'containers/Bundle/Application'
    lines.append(f'installed apps: {sum(1 for _ in apps.iterdir()) if apps.is_dir() else 0}')
    pending = [f'{stage} {path}' for stage, entries in app_journal.pending(data).items() for path in entries]
    lines.append(f'app registrations pending: {", ".join(pending) or "none"}')
    (out / 'migration-state.txt').write_text('\n'.join(lines) + '\n')


def _csstore(data: Path, out: Path, log: host.Log) -> None:
    """Copy LaunchServices' databases and count the WebKit identifiers in them."""
    stores = sorted(
        (data / 'mobile/Containers/Data/InternalDaemon').glob('*/Library/Caches/com.apple.LaunchServices-*.csstore')
    )
    if not stores:
        log('LaunchServices database NOT copied (not found)')
        return
    for store in stores:
        shutil.copyfile(store, out / store.name)
    counts = [f'{ident} {sum(s.read_bytes().count(ident.encode()) for s in stores)}' for ident in WEBKIT_IDS]
    (out / 'lsdb-webkit.txt').write_text('\n'.join(counts) + '\n')


EVERYTHING = Pull()


def pull(data: str | Path, out: str | Path, log: host.Log, what: Pull = EVERYTHING) -> Path:
    """Mirror the unified log, copy the reports and saved files into <out>, and link the logarchive."""
    data, out = Path(data), Path(out)
    out.mkdir(parents=True, exist_ok=True)
    unified = paths.state_dir() / 'device-logs' / 'mirror'
    for name in ('diagnostics', 'uuidtext'):
        files, size = mirror(data / 'db' / name, unified / name)
        log(f'{name}: {files} files ({size >> 20} MiB) new or changed since the last pull')
    _reports(data, out, log, what.launchd)
    _saved(data, out, log, what.kcdata)
    _screenshots(data, out, log)
    _migration_state(data, out)
    if what.csstore:
        _csstore(data, out, log)
    if what.logarchive and (unified / 'diagnostics/timesync').is_dir():
        try:
            boots = mkarchive.build(unified, out / 'boot.logarchive', copy=os.link)
            log('logarchive: {} ({} boots)'.format(out / 'boot.logarchive', len(boots)))
        except SystemExit as error:
            log('logarchive NOT built:', error)
    hwtest.after_pull(data, renamed.data_folder(data), out, log)
    log('logs ->', out)
    return out


def load_spec(path: str | None) -> dict:
    """//install:spec from a runfiles path (bazel run) or a plain path."""
    path = paths.located(path)
    if not path:
        return {'partition_bytes': 32 << 30, 'volumes': {'data': {'uuid': DATA_UUID}}}
    return json.loads(Path(path).read_text())


def data_volume(card: cards.Card, spec_data_uuid: str = DATA_UUID) -> str | None:
    for container in host.apfs_containers():
        for volume in container.get('Volumes', []):
            if volume.get('APFSVolumeUUID', '').upper() == spec_data_uuid:
                store = (container.get('PhysicalStores') or [{}])[0].get('DeviceIdentifier', '')
                if store.startswith(card.disk + 's'):
                    return volume['DeviceIdentifier']
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('disk')
    parser.add_argument('--out')
    parser.add_argument('--corpus', help="precompile corpus to merge into (default: the checkout's corpus.log)")
    parser.add_argument('--harvest-script', help="harvest.py's path in the checkout (//deploy passes it)")
    parser.add_argument('--harvest', action=argparse.BooleanOptionalAction, default=False)
    for flag in ('logarchive', 'csstore', 'launchd'):
        parser.add_argument('--no-' + flag, action='store_true')
    parser.add_argument('--log')
    parser.add_argument('--spec', help="//install:spec (the kernel's volume UUIDs)")
    parser.add_argument('--kcdata', help="XNU's tools/lldbmacros/kcdata.py (decodes a saved panic's stackshot)")
    parser.add_argument('--no-eject', action='store_true', help='leave the card attached (default: unmount and eject)')
    args = parser.parse_args(argv)
    log = host.Log(args.log or paths.run_log('pull-logs', 'switch-ios-install'))
    try:
        host.check_tools(log)
        spec = load_spec(args.spec)
        card = cards.resolve(args.disk)
        _fat, target = cards.check_layout_unprivileged(card, spec)
        volume = data_volume(card, spec['volumes']['data']['uuid'])
        if not volume:
            raise host.HostError('no iPad Data volume {} on {}'.format(spec['volumes']['data']['uuid'], card.disk))
        out = Path(args.out or paths.state_dir() / 'device-logs' / time.strftime('boot-%m%d-%H%M'))
        with cards.mounted(volume, log) as data:
            what = Pull(not args.no_logarchive, not args.no_csstore, not args.no_launchd, paths.located(args.kcdata))
            pull(data, out, log, what)
            if args.harvest:
                harvest(data, out, log, args.harvest_script and str(Path(args.harvest_script).parent), args.corpus)
        if not args.no_eject:
            # As deploy does: the iPad volumes unmounted, then the whole card ejected.
            container = host.container_on(target.identifier)
            if container:
                host.run('diskutil', 'unmountDisk', container['ContainerReference'], log=log, check=False)
            host.run('diskutil', 'eject', card.disk, log=log)
            log('ejected', card.disk)
        return 0
    except cards.RefusedError as error:
        log('REFUSED:', error)
        return 2
    except host.HostError as error:
        log('FAILED:', error)
        return 1


if __name__ == '__main__':
    sys.exit(main())
