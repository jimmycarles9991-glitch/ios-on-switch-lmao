#!/usr/bin/env python3
"""Deploy a new build to an installed card. No sudo needed.

    bazel run //deploy:sd -- <diskN>                       dry run: check the card, show what would change
    bazel run //deploy:sd -- <diskN> --yes                 deploy (type the disk identifier to confirm)

Steps:
  1 pull logs      off unless --pull-logs (or --//deploy:logs): the last boots' logs and logarchive
                   (//deploy:pull_logs; --no-csstore/--no-launchd-log narrow it); --harvest (or --//deploy:harvest)
                   also merges the device's m2v journals into the checkout's corpus.log
  2 boot entry     switchroot/ios/* and bootloader/ini/ios.ini on the FAT: only files that differ are
                   copied, stale files in switchroot/ios/ removed (never the user's bootargs.txt or the install
                   images' NAME.SII.NNN parts), AppleDouble files by name; nothing else on the FAT. Refused
                   when the card's iOS is another kind than the build's (its System volume is not the IPSW's)
  3 overlay        <Data>/Switch/overlay := the package's overlay/; <Data>/Switch/deployed.json := the checkout's
                   commit, which tools/hwtest joins HW Test's reports with (deployed.py); when the overlay's
                   /Applications changed, writes <Data>/datamigrator/migrate so the next boot registers the apps
                   (system_apps.py; --rerun-migration writes it regardless, e.g. after a migration the power cut short;
                   from iOS 27 it also removes LaunchServices' store, whose rebuild is what registers the apps there);
                   copies back from the System volume the staged system apps MobileInstallation still wants that the
                   last migration consumed without installing, and asks for the migration that installs them; also
                   removes the r3 Wi-Fi bring-up's configd Debug logging profile (STALE_LOGGING_PROFILES) if a card
                   still has it; and stages hekate's "Dump Joy-Con BT" (the FAT's switchroot/joycon_mac.ini, only read)
                   as <Data>/Switch/bluetooth/{host-address, joycon-pairing.plist}, removed when the FAT has none
                   (joycon_pairing.py; --no-joycon-pairing skips it)
  4 GPU bundle     <Data>/ngpu/addons/TegraGM20BMetal.{bundle,m2v-seed,raw} from --//deploy:gpu_bundle, via the
                   plugin's stager, which copies only the files whose sha256 the card does not already hold and
                   reads nothing more when the card's list matches the build (its CDHashes must be in the same
                   build's kernel window: deploy the boot package built with that bundle); --remove-gpu removes it
  4a AWDL           --awdl on|off: creates or removes <Data>/Switch/awdl-enable, the empty file SwitchWLAN's AWDL
                   (AirDrop's Wi-Fi link) is off without (read once per boot, after Data mounts)
  4b gallery tiles --reset-gallery-snapshots BUNDLE...: removes those poster extensions' gallery tiles from
                   PosterBoard's data store (poster_snapshots.py), so the next "Add New Wallpaper" renders them again
  5 launchd check  --launchd-check LABEL...: read-only, the jobs Data's launchd database disables (exit 1 if a label
                   is not among them; the kernel rewrites that database at every boot)
  6 eject          unmount the iPad volumes, eject the card (--no-eject keeps it)
"""

import argparse
import json
import os
import plistlib
import sys
import time
from pathlib import Path
from typing import NamedTuple

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.common import paths
from tools.deploy import (
    deployed,
    joycon_pairing,
    poster_snapshots,
    pull_logs,
    system_apps,
)
from tools.install import (
    card as cards,
    host,
    progress,
    renamed,
)

# Logging profiles put on Data by hand during bring-up, which nothing else removes: com.apple.SystemConfiguration at
# Debug, persisted, writes ~8 Debug lines/s to the persisted log (configd SCDynamicStore/IPMonitor, wifid
# SCPreferences).
STALE_LOGGING_PROFILES = ('preferences/Logging/Subsystems/com.apple.SystemConfiguration.plist',)


# SwitchWLANAwdl.cpp kFlag, /private/var/Switch/awdl-enable on the device.
AWDL_FLAG = 'Switch/awdl-enable'


def remove_stale_logging_profiles(data_root: Path, log: host.Log) -> None:
    for name in STALE_LOGGING_PROFILES:
        path = Path(data_root) / name
        if path.is_file():
            path.unlink()
            log('removed stale logging profile', name)


def set_awdl(data_root: Path, on: bool, log: host.Log) -> None:
    flag = Path(data_root) / AWDL_FLAG
    if on:
        flag.parent.mkdir(exist_ok=True)
        flag.touch()
    elif flag.exists():
        flag.unlink()
    log('awdl: {} ({})'.format('on' if flag.exists() else 'off', AWDL_FLAG))


def launchd_check(data: Path, labels: list[str], log: host.Log) -> bool:
    path = Path(data) / 'db/com.apple.xpc.launchd/disabled.plist'
    entries = plistlib.loads(path.read_bytes()) if path.exists() else {}
    disabled = sorted(k for k, v in entries.items() if v is True or (isinstance(v, dict) and v.get('Disabled') is True))
    log(f'launchd: {path} disables {disabled}')
    if (path.parent / 'disabled.501.plist').exists():
        log('launchd: disabled.501.plist is never read by launchd (no per-user database on iOS)')
    missing = [label for label in labels if label not in disabled]
    if missing:
        log(f'launchd: NOT disabled: {missing} (add them to native_switch_watchdog_policy in kern_exec.c)')
    return not missing


def _ios_version(ipsw: str | None) -> str | None:
    return json.loads(Path(ipsw).read_text()).get('version') if ipsw else None


def _ios_build(ipsw: str | None) -> str:
    return json.loads(Path(ipsw).read_text()).get('build', '') if ipsw else ''


def check_kind(card: cards.Card, ipsw: str | None) -> None:
    """Refuse a card whose iOS is another kind than the build's: the build's kernel would not start it."""
    pins = json.loads(Path(ipsw).read_text()) if ipsw else {}
    system = (pins.get('volumes') or {}).get('os_volume_uuid', '').upper()
    if system and not pull_logs.data_volume(card, system):
        raise cards.RefusedError(
            f'{card.disk}: its iOS is not iPadOS {pins["version"]} ({pins["build"]}), which this build is for. '
            'Build for the kind it runs (--config=ios27 for iPadOS 27), or install this kind with install-ios.bin'
        )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('disk')
    parser.add_argument('--yes', action='store_true')
    parser.add_argument('--confirm')
    parser.add_argument('--remove-gpu', action='store_true')
    parser.add_argument('--pull-logs', action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument('--harvest', action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument('--no-csstore', action='store_true')
    parser.add_argument('--no-launchd-log', action='store_true')
    parser.add_argument('--logs-out')
    parser.add_argument('--corpus')
    parser.add_argument('--harvest-script', help="harvest.py's path in the checkout (//deploy passes it)")
    parser.add_argument('--launchd-check', nargs='+', metavar='LABEL', default=None)
    parser.add_argument('--awdl', choices=('on', 'off'))
    parser.add_argument('--rerun-migration', action='store_true')
    parser.add_argument('--reset-gallery-snapshots', nargs='+', metavar='BUNDLE', default=None)
    parser.add_argument('--no-eject', action='store_true')
    parser.add_argument('--no-joycon-pairing', action='store_true')
    parser.add_argument('--boot-package', required=True)
    parser.add_argument('--spec', required=True)
    parser.add_argument('--ipsw', help="the IPSW kind's pins (//install:ipsw_pins): the System volume it installs")
    parser.add_argument('--gpu-bundle')
    parser.add_argument('--stager')
    parser.add_argument('--kcdata')
    parser.add_argument('--log')
    return parser


def _checked(args: argparse.Namespace, log: host.Log) -> tuple[cards.Card, dict, object, object, str]:
    """Check the card and the build; return the card, the spec, its FAT and APFS partitions and its Data volume."""
    host.check_tools(log)
    spec = pull_logs.load_spec(args.spec)
    card = cards.resolve(args.disk)
    fat, target = cards.check_layout_unprivileged(card, spec)
    if target.content != 'Apple_APFS':
        raise cards.RefusedError(f'{card.disk}: {target.identifier} is not installed yet (run //install:sd first)')
    data_uuid = spec['volumes']['data']['uuid']
    data = pull_logs.data_volume(card, data_uuid)
    if not data:
        raise cards.RefusedError(f'{card.disk}: no iPad Data volume {data_uuid}')
    check_kind(card, args.ipsw)
    gpu = Path(args.gpu_bundle) if args.gpu_bundle else None
    if not args.remove_gpu and not (gpu and (gpu.is_file() or (gpu / 'TegraGM20BMetal.trustcache').is_file())):
        raise host.HostError('no GPU bundle (--//deploy:gpu_bundle names //gpu:bundle or a directory like //gpu:base)')
    return card, spec, fat, target, data


def _plan(args: argparse.Namespace, changes: list, removed: list, pairing: dict | None) -> list[str]:
    """Return the steps a deploy would take, for the log."""
    steps = ['pull logs'] if args.pull_logs else []
    steps += ['harvest the m2v journals'] if args.harvest else []
    steps += [f'boot entry: {len(changes)} to copy, {len(removed)} stale', 'overlay']
    if not args.no_joycon_pairing:
        controllers = f'stage {len(pairing["controllers"])} controller(s) for host {pairing["host"]}' if pairing else ''
        steps.append('Joy-Con pairing: ' + (controllers or 'remove any staged dump'))
    steps.append('remove the GPU bundle' if args.remove_gpu else 'GPU bundle: stage what changed')
    steps += [f'AWDL {args.awdl} (<Data>/Switch/awdl-enable)'] if args.awdl else []
    steps += ['run the data migration again on the next boot'] if args.rerun_migration else []
    gallery = args.reset_gallery_snapshots
    steps += ['reset gallery snapshots of {}'.format(' '.join(gallery))] if gallery else []
    steps += [f'launchd check {args.launchd_check}'] if args.launchd_check else []
    return steps + ([] if args.no_eject else ['unmount the iPad volumes and eject'])


def _confirmed(args: argparse.Namespace, card: cards.Card) -> None:
    if args.confirm == card.disk:
        return
    if args.confirm or not sys.stdin.isatty():
        raise cards.RefusedError(f'confirm with --confirm={card.disk} or interactively')
    if input(f'Deploy to {card.disk}? Type {card.disk}: ').strip() != card.disk:
        raise cards.RefusedError('not confirmed')


def _stage_gpu(args: argparse.Namespace, data_root: Path, log: host.Log) -> None:
    command = [sys.executable, args.stager, str(data_root)]
    command += ['--remove'] if args.remove_gpu else ['--build', args.gpu_bundle, '--progress']
    with progress.step('gpu: staging the Metal bundle'):
        result = host.stream(command, progress.ours)
    log('gpu:', (result.stdout + result.stderr).strip())
    if result.returncode:
        raise host.HostError('GPU bundle NOT staged')


class Volumes(NamedTuple):
    """The card's volumes a deploy writes or reads besides Data: the FAT and the System volume."""

    fat: str
    system: str | None


def _restage_system_apps(system: str | None, data_root: Path, log: host.Log) -> bool:
    if not system:
        return False
    with cards.mounted(system, log, read_only=True) as system_root:
        return system_apps.restage(system_root, data_root, log)


def _write(args: argparse.Namespace, data_root: Path, volumes: Volumes, pairing: dict | None, log: host.Log) -> bool:
    """Write the deploy to the mounted Data volume (and the FAT); False when the launchd check fails."""
    renamed.migrate_data(data_root, log)
    out = Path(args.logs_out or paths.state_dir() / 'device-logs' / time.strftime('boot-%m%d-%H%M'))
    if args.pull_logs:
        what = pull_logs.Pull(csstore=not args.no_csstore, launchd=not args.no_launchd_log, kcdata=args.kcdata)
        pull_logs.pull(data_root, out, log, what)
    if args.harvest:
        harvest_dir = args.harvest_script and str(Path(args.harvest_script).parent)
        pull_logs.harvest(data_root, out, log, harvest_dir, args.corpus)
    with cards.mounted(volumes.fat, log) as fat_root:
        cards.sync_boot_entry(fat_root, args.boot_package, log)
    apps = system_apps.digest(data_root)
    cards.sync_overlay(data_root, args.boot_package, log)
    deployed.record(data_root, paths.workspace(), _ios_build(args.ipsw), log)
    restaged = _restage_system_apps(volumes.system, data_root, log)
    rebuild = system_apps.rebuilds_launchservices(_ios_version(args.ipsw))
    system_apps.request_migration(data_root, apps, log, force=args.rerun_migration or restaged, rebuild=rebuild)
    remove_stale_logging_profiles(data_root, log)
    if not args.no_joycon_pairing:
        joycon_pairing.stage(data_root, pairing, log)
    _stage_gpu(args, data_root, log)
    if args.awdl:
        set_awdl(data_root, args.awdl == 'on', log)
    if args.reset_gallery_snapshots:
        poster_snapshots.reset(data_root, args.reset_gallery_snapshots, log)
    return launchd_check(data_root, args.launchd_check, log) if args.launchd_check is not None else True


def _eject(card: cards.Card, target: object, log: host.Log) -> None:
    container = host.container_on(target.identifier)
    if container:
        host.run('diskutil', 'unmountDisk', container['ContainerReference'], log=log, check=False)
    host.run('diskutil', 'eject', card.disk, log=log)
    log('ejected', card.disk)


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    for name in ('boot_package', 'spec', 'ipsw', 'gpu_bundle', 'stager', 'kcdata'):
        setattr(args, name, paths.located(getattr(args, name)))
    log = host.Log(args.log or paths.run_log('deploy', 'switch-ios-install'))
    try:
        with progress.step(f'deploy: checking {args.disk}', unit=None):
            card, spec, fat, target, data = _checked(args, log)
            with cards.mounted(fat.identifier, log) as fat_root:
                changes, removed = cards.sync_boot_entry(fat_root, args.boot_package, log, dry_run=True)
                pairing = None if args.no_joycon_pairing else joycon_pairing.read(fat_root, log)
        log(f'card {card.disk}: FAT {fat.identifier}, iPad Data {data}')
        for step in _plan(args, changes, removed, pairing):
            log('  plan:', step)
        if not args.yes:
            log('dry run: nothing written (add --yes)')
            return 0
        _confirmed(args, card)
        with progress.step(f'deploy: writing to {card.disk}', unit=None):
            preboot = pull_logs.data_volume(card, spec['volumes']['preboot']['uuid'])
            if preboot:
                with cards.mounted(preboot, log) as preboot_root:
                    renamed.migrate_preboot(preboot_root, spec, log)
            system = pull_logs.data_volume(card, spec['volumes'].get('system', {}).get('uuid', '').upper())
            with cards.mounted(data, log) as data_root:
                ok = _write(args, data_root, Volumes(fat.identifier, system), pairing, log)
            if not args.no_eject:
                _eject(card, target, log)
        log('deployed')
        return 0 if ok else 1
    except cards.RefusedError as error:
        log('REFUSED:', error)
        return 2
    except host.HostError as error:
        log('FAILED:', error)
        return 1


if __name__ == '__main__':
    sys.exit(main())
