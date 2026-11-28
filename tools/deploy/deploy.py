#!/usr/bin/env python3
"""Deploy a new build to an installed card. Sudo only on iOS 27, to make Data's Switch/ root's when it is not.

    bazel run //deploy:sd -- <diskN>                       dry run: check the card, show what would change
    bazel run //deploy:sd -- <diskN> --yes                 deploy (type the disk identifier to confirm)

Steps:
  0 Switch/ owner  iOS 27, first: <Data>/Switch (its former name moved to it) root:wheel without group or other write,
                   which the kernel requires before it takes the pretranslated updates (data_owner.py; set under sudo,
                   asked for before the deploy writes anything, only when it is not); the check refuses a mount that
                   does not show the tree (<Data>/Switch/pretranslated) as 0:0, since then owners are not honoured
  1 pull logs      off unless --pull-logs (or --//deploy:logs): the last boots' logs and logarchive
                   (//deploy:pull_logs; --no-csstore/--no-launchd-log narrow it); --harvest (or --//deploy:harvest)
                   also merges the device's m2v journals into the checkout's corpus.log
  2 overlay        <Data>/Switch/overlay := the package's overlay/; on iOS 27 <Data>/Switch/pretranslated-updates :=
                   the build's translations of the card's stale translated System files (translated_tree.py), which
                   the boot package's kernel serves in place of the tree's; <Data>/Switch/deployed.json := the
                   checkout's commit, which tools/hwtest joins HW Test's reports with (deployed.py); each app the
                   overlay's /Applications added, changed or removed goes into <Data>/Switch/app-registration.plist,
                   which switch-appregd applies at the next boot without a migration (app_journal.py; on iOS 17 with
                   switch-no-bt in bootargs.txt nothing starts it, so the deploy asks for the migration instead);
                   --rerun-migration writes <Data>/datamigrator/migrate (system_apps.py, e.g. after a migration the
                   power cut short; from iOS 27 it also removes LaunchServices' store, whose rebuild is what registers
                   the apps there); copies back from the System volume the staged system apps MobileInstallation still
                   wants that the last migration consumed without installing, and asks for the migration that installs
                   them; also removes the r3 Wi-Fi bring-up's configd Debug logging profile (STALE_LOGGING_PROFILES)
                   if a card still has it; and stages hekate's "Dump Joy-Con BT" (the FAT's switchroot/joycon_mac.ini,
                   only read) as <Data>/Switch/bluetooth/{host-address, joycon-pairing.plist}, removed when the FAT has
                   none (joycon_pairing.py; --no-joycon-pairing skips it)
  3 GPU bundle     <Data>/ngpu/addons/TegraGM20BMetal.{bundle,seed,pack} from --//deploy:gpu_bundle, via the
                   plugin's stager, which copies only the files whose sha256 the card does not already hold and
                   reads nothing more when the card's list matches the build (its CDHashes must be in the same
                   build's kernel window: deploy the boot package built with that bundle); --remove-gpu removes it.
                   The stager names what it staged in <Data>/Switch/icon-cache-gpu for the kernel's icon cache drop
  3a AWDL          --awdl on|off: creates or removes <Data>/Switch/awdl-enable, the empty file SwitchWLAN's AWDL
                   (AirDrop's Wi-Fi link) is off without (read once per boot, after Data mounts)
  3b gallery tiles --reset-gallery-snapshots BUNDLE...: removes those poster extensions' gallery tiles from
                   PosterBoard's data store (poster_snapshots.py), so the next "Add New Wallpaper" renders them again
  4 launchd check  --launchd-check LABEL...: read-only, the jobs Data's launchd database disables (exit 1 if a label
                   is not among them; the kernel rewrites that database at every boot)
  5 cryptex        iOS 27: the System cryptex on Preboot := this build's translation (system_update.py, as
                   //deploy:system), written only when its record (tools/install/cryptex_record.py) does not name
                   this build's image; planned in the check, so a card without the room or the image refuses before
                   anything is written
  6 boot entry     last: switchroot/ios/* and bootloader/ini/ios.ini on the FAT: only files that differ are copied,
                   stale files in switchroot/ios/ removed (never the user's bootargs.txt or the install images'
                   NAME.SII.NNN parts), AppleDouble files by name; nothing else on the FAT. Until it is written the
                   card boots its last boot entry over this build's Data; a re-run completes the deploy
  7 eject          unmount the iPad volumes, eject the card (--no-eject keeps it)

The check refuses a card whose iOS is another kind than the build's (its System volume is not the IPSW's), and on iOS
27 one whose translated System files the build neither trusts nor carries an update for (translated_tree.py).

A step besides the overlay, the GPU bundle, Switch/'s owner, the cryptex and the boot entry that fails logs one line
saying so and the deploy goes on to the boot entry and the eject; it then ends FAILED, naming what was not done, with
exit status 1. Those five stop the deploy with one FAILED line, unmounted and not ejected.
"""

import argparse
import json
import os
import plistlib
import shutil
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import NamedTuple

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.common import paths
from tools.deploy import (
    app_journal,
    compiler_inputs,
    data_owner,
    deployed,
    joycon_pairing,
    poster_snapshots,
    pull_logs,
    ssh_keys,
    system_apps,
    system_update,
)
from tools.install import (
    bootstrap,
    card as cards,
    host,
    progress,
    renamed,
    staged_apps,
    translated_tree,
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
    parser.add_argument(
        '--compiler-inputs', type=Path, metavar='DIR', help='stage the matching development HW Test shader fixtures'
    )
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
    parser.add_argument(
        '--ssh-authorized-keys',
        help='public key file or directory staged on dev images (default: $SWITCH_SSH_AUTHORIZED_KEYS or ~/.ssh/*.pub)',
    )
    parser.add_argument(
        '--no-ssh-authorized-keys',
        action='store_true',
        help='do not stage SSH keys and remove any staged authorized_keys',
    )
    parser.add_argument('--boot-package', required=True)
    parser.add_argument('--spec', required=True)
    parser.add_argument('--ipsw', help="the IPSW kind's pins (//install:ipsw_pins): the System volume it installs")
    parser.add_argument(
        '--pretranslated-trust', help="iOS 27: this build's os.trustcache (//ipsw:pretranslated_os_trust)"
    )
    parser.add_argument(
        '--pretranslated-updates', help="iOS 27: this build's translated updates (//install:pretranslated_updates)"
    )
    parser.add_argument('--pretranslated-pins', help='iOS 27: {file name: sha256} the boot package carries')
    parser.add_argument(
        '--system-image', help="iOS 27: this build's System cryptex (//install:translated_system_cryptex)"
    )
    parser.add_argument('--system-trusted', action='append', default=[], help=' and '.join(system_update.TRUSTED))
    parser.add_argument('--gpu-bundle')
    parser.add_argument('--stager')
    parser.add_argument('--kcdata')
    parser.add_argument('--log')
    return parser


def _checked(args: argparse.Namespace, log: host.Log) -> tuple[cards.Card, dict, object, object, str]:
    """Check the card and the build; return the card, the spec, its FAT and APFS partitions and its Data volume."""
    host.check_tools(log)
    spec = pull_logs.load_spec(args.spec)
    if args.compiler_inputs:
        args.shader_fixtures = compiler_inputs.load(
            args.compiler_inputs, Path(args.boot_package), _ios_build(args.ipsw)
        )
    source = deployed.provenance(Path(args.boot_package))
    if source and source.get('build') and source['build'] != _ios_build(args.ipsw):
        raise cards.RefusedError(f'{args.boot_package}: snapshot is for {source["build"]}, not {_ios_build(args.ipsw)}')
    card = cards.resolve(args.disk)
    fat, target = cards.check_layout_unprivileged(card, spec)
    if target.content != 'Apple_APFS':
        raise cards.RefusedError(f'{card.disk}: {target.identifier} is not installed yet (run //install:sd first)')
    data_uuid = spec['volumes']['data']['uuid']
    data = pull_logs.data_volume(card, data_uuid)
    if not data:
        raise cards.RefusedError(f'{card.disk}: no iPad Data volume {data_uuid}')
    check_kind(card, args.ipsw)
    if args.system_image:
        pins = json.loads(Path(args.pretranslated_pins).read_text())
        system_update.check_translation([Path(path) for path in args.system_trusted], pins)
    args.pretranslated_wanted = None
    args.data_owner = None
    if args.pretranslated_trust:
        with cards.mounted(data, log, read_only=True) as data_root:
            args.pretranslated_wanted = translated_tree.plan(
                data_root, Path(args.pretranslated_trust), Path(args.pretranslated_updates)
            )
        args.data_owner = data_owner.read(data, log)
    args.cryptex = None
    if args.system_image:
        image = system_update.Image(Path(args.system_image), Path(args.system_image + '.sha256').read_text().strip())
        args.cryptex = (image, system_update.planned_on_card(card, spec, image, target, log))
    gpu = Path(args.gpu_bundle) if args.gpu_bundle else None
    if not args.remove_gpu and not (gpu and (gpu.is_file() or (gpu / 'gpu.stage').is_file())):
        raise host.HostError('no GPU bundle (--//deploy:gpu_bundle names //gpu:bundle or a stage tree like //gpu:base)')
    return card, spec, fat, target, data


def _plan(
    args: argparse.Namespace,
    changes: list,
    removed: list,
    pairing: dict | None,
    key_plan: ssh_keys.KeyPlan | None = None,
) -> list[str]:
    """Return the steps a deploy would take, for the log."""
    steps = ['pull logs'] if args.pull_logs else []
    steps += ['harvest the m2v journals'] if args.harvest else []
    steps += ['overlay']
    if key_plan is not None:
        steps.append(ssh_keys.plan_description(key_plan))
    wanted = getattr(args, 'pretranslated_wanted', None)
    if wanted is not None:
        steps.append(
            'pretranslated updates: ' + (', '.join(sorted(wanted)) if wanted else 'none (the tree is current)')
        )
    if not args.no_joycon_pairing:
        controllers = f'stage {len(pairing["controllers"])} controller(s) for host {pairing["host"]}' if pairing else ''
        steps.append('Joy-Con pairing: ' + (controllers or 'remove any staged dump'))
    steps.append(
        f'stage five compiler comparisons from {args.compiler_inputs}'
        if args.compiler_inputs
        else 'remove any staged compiler comparisons'
    )
    steps.append('remove the GPU bundle' if args.remove_gpu else 'GPU bundle: stage what changed')
    steps += [f'AWDL {args.awdl} (<Data>/Switch/awdl-enable)'] if args.awdl else []
    steps += ['run the data migration again on the next boot'] if args.rerun_migration else []
    gallery = args.reset_gallery_snapshots
    steps += ['reset gallery snapshots of {}'.format(' '.join(gallery))] if gallery else []
    steps += [f'launchd check {args.launchd_check}'] if args.launchd_check else []
    if getattr(args, 'pretranslated_wanted', None) is not None:
        steps.append(data_owner.planned(getattr(args, 'data_owner', None)))
    cryptex = getattr(args, 'cryptex', None)
    if cryptex:
        steps.append('System cryptex: ' + ("the card's is this build's" if cryptex[1].same else "write this build's"))
    steps.append(f'boot entry, last: {len(changes)} to copy, {len(removed)} stale')
    return steps + ([] if args.no_eject else ['unmount the iPad volumes and eject'])


def _confirmed(args: argparse.Namespace, card: cards.Card) -> None:
    if args.confirm == card.disk:
        return
    if args.confirm or not sys.stdin.isatty():
        raise cards.RefusedError(f'confirm with --confirm={card.disk} or interactively')
    if input(f'Deploy to {card.disk}? Type {card.disk}: ').strip() != card.disk:
        raise cards.RefusedError('not confirmed')


# The plugin's cache folders before the driver left raw/ (its cacheCandidates then, harvest.py's --volume list): the
# plugin makes the new ones afresh and nothing reads these. Containers a process closed (no read and search) are left.
RENAMED_CACHES = (
    'tmp/m2v',
    'tmp/m2v-*',
    'mobile/tmp/*/m2v',
    'mobile/Library/Caches/m2v',
    'root/Library/Caches/m2v',
    'folders/*/*/C/m2v',
    'folders/*/*/T/m2v',
    'containers/Data/System/*/Library/Caches/m2v',
    'containers/Data/System/*/Library/Caches/m2v-*',
    'containers/Data/System/*/tmp/m2v',
    'mobile/Containers/Data/*/*/Library/Caches/m2v',
    'mobile/Containers/Data/*/*/Library/Caches/m2v-*',
    'mobile/Containers/Data/*/*/tmp/m2v',
)


def _remove_renamed_caches(data_root: Path, log: host.Log) -> None:
    for pattern in RENAMED_CACHES:
        for folder in sorted(data_root.glob(pattern)):
            if folder.is_dir() and not folder.is_symlink():
                shutil.rmtree(folder)
                log('gpu: removed', folder.relative_to(data_root), '(the cache folder before the rename)')


def _stage_gpu(args: argparse.Namespace, data_root: Path, log: host.Log) -> None:
    _remove_renamed_caches(data_root, log)
    command = [sys.executable, args.stager, str(data_root)]
    command += ['--remove'] if args.remove_gpu else ['--build', args.gpu_bundle, '--progress']
    with progress.step('gpu: staging the Metal bundle'):
        result = host.stream(command, progress.ours)
    log('gpu:', (result.stdout + result.stderr).strip())
    if result.returncode:
        raise host.HostError('GPU bundle NOT staged')


class Volumes(NamedTuple):
    """What a deploy's Data step reads elsewhere: the System volume, and the boot-args on the FAT."""

    system: str | None
    boot_args: str


def _boot_args(fat_root: Path) -> str:
    """Return the card's own boot-args, switchroot/ios/bootargs.txt, which the loader appends to the build's."""
    try:
        return (Path(fat_root) / cards.BOOT_ENTRY / cards.BOOT_ARGS).read_text(errors='replace')
    except OSError:
        return ''


def _restage_system_apps(system: str | None, data_root: Path, log: host.Log) -> bool:
    if not system:
        return False
    with cards.mounted(system, log, read_only=True) as system_root:
        return system_apps.restage(system_root, data_root, log)


class _Steps:
    """The steps that run once the card is being written, besides the boot entry, the overlay and the GPU bundle.

    One that cannot do its work logs one line and is counted, and the rest, the unmount and the eject still run: a
    deploy that stopped there left a card whose kernel no longer matched its GPU bundle, or a card never ejected.
    """

    def __init__(self, log: host.Log) -> None:
        self.log = log
        self.not_done: list[str] = []

    def run[T](self, what: str, step: Callable[..., T], *args: object) -> T | None:
        try:
            return step(*args)
        except (OSError, ValueError, host.HostError) as error:
            self.log(f'{what}: NOT done: {error}')
            self.not_done.append(what)
            return None


def _stage_ssh_keys(args: argparse.Namespace, data_root: Path, log: host.Log) -> bool:
    options = ssh_keys.Options(path=args.ssh_authorized_keys, disabled=args.no_ssh_authorized_keys)
    plan = ssh_keys.load(Path(args.boot_package), log, options)
    return ssh_keys.stage(data_root, plan, log)


def _write(
    args: argparse.Namespace, data_root: Path, volumes: Volumes, pairing: dict | None, log: host.Log
) -> list[str]:
    """Write the deploy to the mounted Data volume; return the steps not done (a failed launchd check)."""
    renamed.migrate_data(data_root, log)
    out = Path(args.logs_out or paths.state_dir() / 'device-logs' / time.strftime('boot-%m%d-%H%M'))
    if args.pull_logs:
        what = pull_logs.Pull(csstore=not args.no_csstore, launchd=not args.no_launchd_log, kcdata=args.kcdata)
        pull_logs.pull(data_root, out, log, what)
    if args.harvest:
        harvest_dir = args.harvest_script and str(Path(args.harvest_script).parent)
        pull_logs.harvest(data_root, out, log, harvest_dir, args.corpus)
    steps = _Steps(log)
    apps = steps.run('system apps registration', system_apps.apps, data_root)
    cards.sync_overlay(data_root, args.boot_package, log)
    wanted = getattr(args, 'pretranslated_wanted', None)
    if wanted is not None:
        translated_tree.stage(data_root, Path(args.pretranslated_updates), wanted, log)
    pins = json.loads(Path(args.ipsw).read_text()) if args.ipsw else {}
    # Capture an interrupted native migration before bootstrap.stage unlinks Switch/system-app-migration.
    interrupted = steps.run('interrupted migration', system_apps.interrupted_migration, data_root)
    steps.run('bootstrap migration', bootstrap.stage, data_root, args.boot_package, pins, log)
    steps.run(
        'compiler inputs', compiler_inputs.stage, data_root, args.shader_fixtures if args.compiler_inputs else None
    )
    steps.run(
        'deployed.json',
        deployed.record,
        data_root,
        paths.workspace(),
        _ios_build(args.ipsw),
        log,
        Path(args.boot_package),
    )
    restaged = steps.run('system apps restage', _restage_system_apps, volumes.system, data_root, log)
    if apps is not None:
        next_boot = app_journal.NextBoot(_ios_version(args.ipsw), volumes.boot_args)
        force = any((args.rerun_migration, restaged, interrupted))
        steps.run('system apps registration', system_apps.register, data_root, apps, log, next_boot, force)
    steps.run('stale logging profiles', remove_stale_logging_profiles, data_root, log)
    steps.run('SSH authorized_keys', _stage_ssh_keys, args, data_root, log)
    if not args.no_joycon_pairing:
        steps.run('Joy-Con pairing', joycon_pairing.stage, data_root, pairing, log)
    _stage_gpu(args, data_root, log)
    if args.awdl:
        steps.run('AWDL', set_awdl, data_root, args.awdl == 'on', log)
    if args.reset_gallery_snapshots:
        steps.run('gallery snapshots', poster_snapshots.reset, data_root, args.reset_gallery_snapshots, log)
    if args.launchd_check is not None:
        checked = steps.run('launchd check', launchd_check, data_root, args.launchd_check, log)
        if checked is False:
            steps.not_done.append('launchd check')
    return steps.not_done


def _update_system_cryptex(args: argparse.Namespace, preboot: str | None, target: object, log: host.Log) -> None:
    """Before the boot entry: the card's System cryptex as this build translated it, or the deploy fails."""
    image, update = args.cryptex
    if not preboot:
        raise host.HostError("no Preboot volume: the card's System cryptex was not brought to this build")
    system_update.write_on_card(preboot, update, image, target, log)


def _own_switch(data: str, log: host.Log) -> None:
    """Switch/ under its own name and root's, before any other Data write."""
    with cards.mounted(data, log) as data_root:
        renamed.migrate_data(data_root, log)
        (Path(data_root) / renamed.DATA_FOLDER).mkdir(exist_ok=True)
    data_owner.ensure(data, log)


def _write_boot_entry(args: argparse.Namespace, fat: str, log: host.Log) -> None:
    with cards.mounted(fat, log) as fat_root:
        cards.sync_boot_entry(fat_root, args.boot_package, log)


def _eject(card: cards.Card, target: object, log: host.Log) -> None:
    container = host.container_on(target.identifier)
    if container:
        host.run('diskutil', 'unmountDisk', container['ContainerReference'], log=log, check=False)
    host.run('diskutil', 'eject', card.disk, log=log)
    log('ejected', card.disk)


def _located(args: argparse.Namespace) -> None:
    for name in (
        'boot_package',
        'spec',
        'ipsw',
        'pretranslated_trust',
        'pretranslated_updates',
        'pretranslated_pins',
        'system_image',
        'gpu_bundle',
        'stager',
        'kcdata',
    ):
        setattr(args, name, paths.located(getattr(args, name)))
    args.system_trusted = [paths.located(path) for path in args.system_trusted]


class Checked(NamedTuple):
    """What the check found: the card, its spec, FAT, APFS partition, Data volume, the FAT's pairing and boot-args."""

    card: cards.Card
    spec: dict
    fat: object
    target: object
    data: str
    pairing: dict | None
    boot_args: str


def _write_card(args: argparse.Namespace, checked: Checked, log: host.Log) -> list[str]:
    """Write the card, the boot entry last; return the steps not done."""
    card, spec = checked.card, checked.spec
    if args.pretranslated_trust:
        _own_switch(checked.data, log)
    preboot = pull_logs.data_volume(card, spec['volumes']['preboot']['uuid'])
    if preboot:
        with cards.mounted(preboot, log) as preboot_root:
            renamed.migrate_preboot(preboot_root, spec, log)
    system = pull_logs.data_volume(card, spec['volumes'].get('system', {}).get('uuid', '').upper())
    if staged_apps.stage_on_card(checked.data, Path(args.boot_package), log):
        args.rerun_migration = True
    with cards.mounted(checked.data, log) as data_root:
        not_done = _write(args, data_root, Volumes(system, checked.boot_args), checked.pairing, log)
    if args.system_image:
        _update_system_cryptex(args, preboot, checked.target, log)
    _write_boot_entry(args, checked.fat.identifier, log)
    if not args.no_eject:
        _eject(card, checked.target, log)
    return not_done


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    _located(args)
    log = host.Log(args.log or paths.run_log('deploy', 'switch-ios-install'))
    try:
        with progress.step(f'deploy: checking {args.disk}', unit=None):
            card, spec, fat, target, data = _checked(args, log)
            with cards.mounted(fat.identifier, log) as fat_root:
                changes, removed = cards.sync_boot_entry(fat_root, args.boot_package, log, dry_run=True)
                pairing = None if args.no_joycon_pairing else joycon_pairing.read(fat_root, log)
                checked = Checked(card, spec, fat, target, data, pairing, _boot_args(fat_root))
            ssh_options = ssh_keys.Options(path=args.ssh_authorized_keys, disabled=args.no_ssh_authorized_keys)
            key_plan = ssh_keys.load(Path(args.boot_package), log, ssh_options)
        log(f'card {card.disk}: FAT {fat.identifier}, iPad Data {data}')
        for step in _plan(args, changes, removed, pairing, key_plan):
            log('  plan:', step)
        if not args.yes:
            log('dry run: nothing written (add --yes)')
            return 0
        _confirmed(args, card)
        if args.pretranslated_trust:
            data_owner.authorize(getattr(args, 'data_owner', None), log)
        with progress.step(f'deploy: writing to {card.disk}', unit=None):
            not_done = _write_card(args, checked, log)
        if not_done:
            log('FAILED: not done:', ', '.join(not_done))
            return 1
        log('deployed')
        return 0
    except cards.RefusedError as error:
        log('REFUSED:', error)
        return 2
    except (host.HostError, OSError, ValueError) as error:
        log('FAILED:', error)
        return 1


if __name__ == '__main__':
    sys.exit(main())
