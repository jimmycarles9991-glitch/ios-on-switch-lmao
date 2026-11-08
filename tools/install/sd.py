#!/usr/bin/env python3
"""Install iPadOS 17.7.11 (21H461) for ios-on-switch-lmao onto a Nintendo Switch SD card.

    bazel run //install:sd -- <diskN>                 dry run: check the card, print the plan, write nothing
    bazel run //install:sd -- <diskN> --yes           do it (asks you to type diskN, then runs itself under sudo)

Writes ONLY: the card's Linux or Android partition, at whatever size hekate made it (an APFS container: System, Data,
Preboot), that partition's GPT entry
(type APFS, name iOS; primary and backup copies, nothing else in the partition table, never the hybrid MBR),
and on the FAT: switchroot/ios/* and bootloader/ini/ios.ini. See tools/install/card.py for the guards.

Steps, each skipped when the card already has its result (so a rerun only does what is missing or changed):
  1 compose  In a scratch image (not the card): tools/install/compose.py.
  2 write    Its written blocks onto the partition, then the GPT entry's type to APFS.
  3 reprobe  macOS must re-read the card: a disk image is detached and attached again; a real card is ejected and
             you reinsert it and run the same command again (it continues at step 4).
  4 populate Data from the System volume's /private/var (only when Data is empty, or with --reseed-data); the two
             cryptex images into Preboot (by sha256 against the IPSW pins); the userland overlay.
  5 boot     The hekate entry on the FAT (only files that differ; stale files in switchroot/ios removed).
  6 verify   One APFS partition of the right size, the partition table unchanged but for our entry, the volume UUIDs,
             fsck_apfs -n clean. Then the card is ejected (--no-eject keeps it).
"""

import argparse
import json
import os
import sys
import time
import uuid
from pathlib import Path
from typing import NoReturn

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.common import paths, sudo
from tools.install import (
    apfs,
    card as cards,
    compose as composer,
    gpt,
    host,
    owners as ownership,
    populate,
    target as targets,
)

COPY_CHUNK = composer.COPY_CHUNK
SEEK_HOLE, SEEK_DATA = getattr(os, 'SEEK_HOLE', 3), getattr(os, 'SEEK_DATA', 4)


def resolve_inputs(args: argparse.Namespace) -> None:
    """Runfiles paths (from `bazel run`) to absolute paths, so a sudo re-exec needs no Bazel environment."""
    for name in ('spec', 'ipsw', 'os_dmg', 'system_cryptex', 'app_cryptex', 'boot_package'):
        setattr(args, name, paths.located(getattr(args, name)))


class Installer:
    def __init__(self, args: argparse.Namespace, log: host.Log):
        self.args, self.log = args, log
        self.spec = json.loads(Path(args.spec).read_text())
        self.pins = json.loads(Path(args.ipsw).read_text())
        self.want = {k: v['uuid'] for k, v in self.spec['volumes'].items()}
        self.card = None

    def inspect(self) -> str:
        self.card = cards.resolve(self.args.disk)
        cards.read_table(self.card)
        cards.check_layout(self.card, self.spec, self.args.partition)
        return self.state()

    def container_volumes(self) -> list[tuple[uuid.UUID, int, str]] | None:
        """[(uuid, role, name)] read from the target partition itself (works whether or not macOS attached it)."""
        fd = os.open(self.card.raw, os.O_RDONLY)
        try:
            return apfs.Container(fd, self.card.target.first * gpt.SECTOR).volumes()
        except apfs.ApfsError:
            return None
        finally:
            os.close(fd)

    def ours(self, found: list[tuple[uuid.UUID, int, str]] | None) -> bool:
        return composer.ours(self.spec, found)

    def state(self) -> str:
        target, found = self.card.target, self.container_volumes()
        if self.args.reinstall_container:
            # Never write under a container macOS has attached: make it Linux-typed again first (macOS detaches it on
            # the reprobe), then compose as on a fresh card.
            return 'detach' if target.type == gpt.APFS else 'compose'
        if target.type == gpt.APFS:
            if not self.ours(found):
                raise cards.RefusedError(
                    f'{target.describe()} is APFS but not an ios-on-switch-lmao container (volumes {found}); '
                    'rerun with --reinstall-container to replace it'
                )
            if not cards.volumes(self.card):
                return 'reprobe'  # written, retyped, but macOS has not attached it yet
            return 'populate'
        if self.ours(found):
            return 'retype'  # written by an interrupted run, still Linux-typed
        return 'compose'

    def plan(self, state: str) -> list[str]:
        return {
            'detach': [
                'retype the GPT entry back to Linux data so macOS lets go of the old container',
                'reprobe',
                'compose',
                'write',
                'retype to APFS',
                'reprobe',
                'populate',
                'boot entry',
                'verify',
            ],
            'compose': [
                f'compose the container in {self.scratch_dir()} (about {7} GB free needed)',
                f'write it to {self.card.slice(self.card.target)} ({self.card.target.describe()})',
                'retype the GPT entry to APFS, name iOS',
                'reprobe',
                'populate',
                'boot entry',
                'verify',
            ],
            'retype': ['retype the GPT entry to APFS, name iOS', 'reprobe', 'populate', 'boot entry', 'verify'],
            'reprobe': ['reprobe', 'populate', 'boot entry', 'verify'],
            'populate': ['populate (only what differs)', 'boot entry', 'verify'],
        }[state]

    def scratch_dir(self) -> Path:
        return Path(self.args.scratch or (Path.home() / 'Library/Caches/switch-ios-install'))

    def compose(self) -> tuple[Path, int, int]:
        image = sudo.user_directory(self.scratch_dir()) / 'compose.img'
        self.log("compose: attaching the IPSW's OS image", self.args.os_dmg)
        try:
            offset, os_bytes = composer.compose(image, self.args.os_dmg, self.card.target.bytes, self.spec, self.log)
        finally:
            if image.exists():
                sudo.give_back(image)
        return image, offset, os_bytes

    def write(self, image: Path, part_offset: int, os_bytes: int) -> None:
        """Write the scratch partition's written regions onto the target partition.

        [0, os_bytes) is written whole (the OS container, checkpoint areas included, so nothing of an earlier container
        on the card survives there); beyond it only the extents the scratch file has data in (blocks the grown
        container wrote).
        """
        target_offset = self.card.target.first * gpt.SECTOR
        size = self.card.target.bytes
        src = os.open(image, os.O_RDWR)
        dst = os.open(self.card.raw, os.O_RDWR)
        written = 0
        try:
            extents = [(0, os_bytes)]
            position = os_bytes
            while position < size:
                try:
                    data = os.lseek(src, part_offset + position, SEEK_DATA) - part_offset
                except OSError:
                    break
                if data >= size:
                    break
                hole = min(os.lseek(src, part_offset + data, SEEK_HOLE) - part_offset, size)
                extents.append((data, hole))
                position = hole
            for start, stop in extents:
                at = start - start % 4096
                end = stop + -stop % 4096
                while at < min(end, size):
                    n = min(COPY_CHUNK, min(end, size) - at)
                    chunk = os.pread(src, n, part_offset + at)
                    if len(chunk) != n or os.pwrite(dst, chunk, target_offset + at) != n:
                        raise host.HostError(f'copy to {self.card.raw} failed at {at}')
                    composer.punch(src, part_offset + at, n)  # the scratch shrinks as the card fills
                    at += n
                    written += n
            os.fsync(dst)
        finally:
            os.close(src)
            os.close(dst)
        self.log(f'write: {written} bytes in {len(extents)} extents onto {self.card.slice(self.card.target)}')

    def retype(self, kind: uuid.UUID = gpt.APFS, name: str = 'iOS') -> None:
        fat = self.card.slice(self.card.fat)
        if host.mount_point(fat):
            host.run('diskutil', 'unmount', fat, log=self.log)
        container = host.container_on(self.card.slice(self.card.target))
        if container:
            host.run('diskutil', 'unmountDisk', container['ContainerReference'], log=self.log, check=False)
        before = cards.table_snapshot(self.card)
        writes = gpt.edit(self.card.table, self.card.target.index, kind, name)
        fd = os.open(self.card.raw, os.O_RDWR)
        try:
            gpt.apply(fd, writes)
        finally:
            os.close(fd)
        cards.read_table(self.card)
        after = cards.table_snapshot(self.card)
        if before[: gpt.SECTOR] != after[: gpt.SECTOR]:
            raise host.HostError('the MBR changed (it must not)')
        self.log(
            f'retype: {self.card.target.describe()} -> {gpt.NAMES[kind]} {name!r} ({len(writes)} sectors rewritten)'
        )

    def reprobe(self, expect_container: bool = True) -> bool:
        if self.card.image:
            image = self.card.image
            host.run('hdiutil', 'detach', self.card.disk, log=self.log)
            fmt = host.run('hdiutil', 'imageinfo', '-format', image).strip()
            args = ['attach', '-plist', '-nomount']
            if fmt.startswith('RAW'):
                args += ['-imagekey', 'diskimage-class=CRawDiskImage']
            out = host.plist('hdiutil', *args, image, log=self.log)
            disk = host.whole_disk_of(out['system-entities'])
            self.log('reprobe: image re-attached as', disk)
            self.args.disk = disk
            for _ in range(30):
                self.inspect()
                if bool(cards.volumes(self.card)) == expect_container:
                    return True
                time.sleep(1)
            raise host.HostError(
                'macOS did {}attach the container on {}'.format('not ' if expect_container else '', disk)
            )
        host.run('diskutil', 'eject', self.card.disk, log=self.log)
        self.log(
            '\nThe card is ejected so macOS reads its new partition table. Reinsert it and run the same command '
            'again; it continues where it stopped.'
        )
        return False

    def populate(self) -> None:
        vols = cards.volumes(self.card)
        missing = [k for k, u in self.want.items() if u not in vols]
        if missing:
            raise host.HostError(f'volumes {missing} not attached (have {sorted(vols)})')
        dev = {k: vols[u]['DeviceIdentifier'] for k, u in self.want.items()}
        owners = ownership.Immediate() if os.geteuid() == 0 else None
        if not owners and not self.args.unprivileged_test:
            raise host.HostError('populating needs root (file owners): run with sudo')
        with (
            cards.mounted(dev['system'], self.log, read_only=True) as system,
            cards.mounted(dev['data'], self.log, owners=True) as data,
            cards.mounted(dev['preboot'], self.log, owners=True) as preboot,
        ):
            if not (system / 'private/preboot').is_dir():
                raise host.HostError('the System volume has no /private/preboot (the kernel mounts Preboot there)')
            populate.seed_data(system, data, owners, self.log, self.args.reseed_data)
            sources = {'system_cryptex': self.args.system_cryptex, 'app_cryptex': self.args.app_cryptex}
            populate.stage_cryptexes(
                preboot, self.spec, populate.cryptex_images(self.spec, self.pins, sources), owners, self.log
            )
            if self.args.boot_package:
                cards.sync_overlay(data, self.args.boot_package, self.log)

    def boot_entry(self) -> None:
        if not self.args.boot_package:
            self.log('boot entry: no boot package given; skipped')
            return
        fat = self.card.slice(self.card.fat)
        with cards.mounted(fat, self.log) as root:
            cards.sync_boot_entry(root, self.args.boot_package, self.log)

    def verify(self, snapshot: bytes, retyped: bool) -> None:
        self.inspect()
        after = cards.table_snapshot(self.card)
        changed = cards.changed_outside(self.card, snapshot, after, self.card.target.index if retyped else None)
        if changed:
            raise host.HostError(
                f'verify: partition-table bytes changed outside the target entry: offsets {changed[:16]}'
            )
        self.log(
            'verify: partition table: {} (MBR untouched)'.format(
                'only the target entry and the two headers changed' if retyped else 'unchanged'
            )
        )
        apfs_entries = [e for e in self.card.table.used if e.type == gpt.APFS]
        if len(apfs_entries) != 1 or apfs_entries[0].index != self.card.target.index:
            raise host.HostError(f'verify: APFS entries {[e.describe() for e in apfs_entries]}')
        vols = cards.volumes(self.card)
        for key, want in self.want.items():
            if want not in vols:
                raise host.HostError(f'verify: volume {key} {want} missing')
        composer.fsck(self.card.slice(self.card.target), 'card', self.log)
        self.log(
            'verify: one APFS partition ({} bytes), volumes {}'.format(
                self.card.target.bytes, ', '.join(f'{k} {v}' for k, v in self.want.items())
            )
        )

    def check_unprivileged(self) -> None:
        card = cards.resolve(self.args.disk)
        fat, target = cards.check_layout_unprivileged(card, self.spec)
        self.log(
            f'card: {card.disk} ({card.size} bytes); FAT {fat.identifier}, target {target.identifier} '
            f'{target.name!r} ({target.size} bytes, {target.content})'
        )
        self.log(
            "dry run: the layout passes diskutil's checks. Reading the partition table itself (hybrid MBR, "
            'both GPT copies) and the container needs root: --yes runs the full checks under sudo before '
            'writing anything.'
        )

    def rewrite(self, state: str, snapshot: bytes) -> tuple[bytes, bool] | None:
        """Take the card from `state` to an attached container.

        Returns the partition table to verify against and whether the target entry was retyped since, or None when
        the card was ejected to be reinserted.
        """
        retyped = False
        if state == 'detach':
            self.retype(gpt.LINUX_DATA, targets.SETUP_NAME)
            if not self.reprobe(expect_container=False):
                return None
            snapshot = cards.table_snapshot(self.card)
            state = 'compose'
        if state == 'compose':
            if host.container_on(self.card.slice(self.card.target)):
                raise host.HostError(
                    f'macOS has a container attached on {self.card.slice(self.card.target)}: refusing to write under it'
                )
            image, offset, os_bytes = self.compose()
            try:
                self.write(image, offset, os_bytes)
            finally:
                if not self.args.keep_scratch:
                    image.unlink()
            state = 'retype'
        if state == 'retype':
            self.retype()
            retyped = True
            state = 'reprobe'
        if state == 'reprobe' and not self.reprobe():
            return None
        return snapshot, retyped

    def run(self) -> int:
        try:
            state = self.inspect()
        except host.HostError as error:
            if 'needs root' not in str(error) or self.args.yes:
                raise
            self.check_unprivileged()
            return 0
        self.log(
            'card: {} ({} bytes{}), target {}'.format(
                self.card.disk,
                self.card.size,
                ', image ' + self.card.image if self.card.image else '',
                self.card.target.describe(),
            )
        )
        self.log('state:', state)
        for step in self.plan(state):
            self.log('  plan:', step)
        if not self.args.yes:
            self.log('dry run: nothing written (add --yes to install)')
            return 0
        snapshot = cards.table_snapshot(self.card)
        backup = Path(str(self.log.path) + '.partition-table.bin')
        backup.write_bytes(snapshot)
        self.log('partition table backed up to', backup)
        written = self.rewrite(state, snapshot)
        if written is None:
            return 0
        snapshot, retyped = written
        self.populate()
        self.boot_entry()
        self.verify(snapshot, retyped)
        if not self.card.image and not self.args.no_eject:
            host.run('diskutil', 'eject', self.card.disk, log=self.log)
            self.log('ejected', self.card.disk)
        self.log('done')
        return 0


def reexec_as_root(args: argparse.Namespace, argv: list[str]) -> NoReturn:
    """After confirmation: the same command under sudo, with absolute input paths and the confirmation."""
    passthrough = [a for a in argv if not a.startswith('--confirm')]
    for name in ('spec', 'ipsw', 'os_dmg', 'system_cryptex', 'app_cryptex', 'boot_package', 'scratch', 'log'):
        value = getattr(args, name)
        flag = '--' + name.replace('_', '-')
        passthrough = [a for a in passthrough if not a.startswith(flag + '=')]
        if value:
            passthrough.append(f'{flag}={value}')
    sudo.rerun(__file__, [*passthrough, f'--confirm={args.confirm}'])


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('disk', help="the card's whole disk, e.g. disk4 (see `diskutil list external`)")
    parser.add_argument('--yes', action='store_true', help='install (default: dry run)')
    parser.add_argument('--confirm', help='the disk identifier, instead of typing it (scripts and tests)')
    parser.add_argument('--reseed-data', action='store_true', help="replace Data's contents with a fresh seed")
    parser.add_argument('--reinstall-container', action='store_true', help='rewrite the container even if present')
    parser.add_argument('--partition', help='the partition to install to, by GPT name, when the card has several')
    parser.add_argument('--no-eject', action='store_true')
    parser.add_argument('--keep-scratch', action='store_true')
    parser.add_argument(
        '--unprivileged-test',
        action='store_true',
        help='disk images you own only: run without root, skipping file ownership (NOT bootable)',
    )
    parser.add_argument(
        '--scratch',
        help='scratch directory for the composed container (default ~/Library/Caches/'
        'switch-install; needs about 7 GB free)',
    )
    parser.add_argument('--log')
    parser.add_argument('--spec', required=True)
    parser.add_argument('--ipsw', required=True)
    parser.add_argument('--os-dmg', required=True)
    parser.add_argument('--system-cryptex', required=True)
    parser.add_argument('--app-cryptex', required=True)
    parser.add_argument('--boot-package')
    args = parser.parse_args(argv)
    resolve_inputs(args)
    args.log = args.log or str(paths.run_log('install', 'switch-ios-install'))
    log = host.Log(args.log)
    try:
        host.check_tools(log)
        installer = Installer(args, log)
        if args.yes:
            card = cards.resolve(args.disk)
            if args.unprivileged_test and not card.image:
                raise cards.RefusedError('--unprivileged-test is only for disk images')
            cards.confirm(args, card.disk, f"This rewrites {card.disk}'s userdata partition.")
            if os.geteuid() != 0 and not args.unprivileged_test:
                args.scratch = str(installer.scratch_dir())
                reexec_as_root(args, argv)
        return installer.run()
    except cards.RefusedError as error:
        log('REFUSED:', error)
        return 2
    except (host.HostError, apfs.ApfsError, gpt.GptError) as error:
        log('FAILED:', error)
        return 1
    finally:
        log.quiet('log:', args.log)


if __name__ == '__main__':
    sys.exit(main())
