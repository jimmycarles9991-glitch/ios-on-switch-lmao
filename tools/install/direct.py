#!/usr/bin/env python3
"""Install an image from //:image's folder (out/sd) straight onto the card's iOS partition, from the Mac.

    bazel run //:image_to_card -- diskN --direct                dry run: the checks and the plan, nothing written
    bazel run //:image_to_card -- diskN --direct --yes          do it (type diskN to confirm; it reruns under sudo)
    bazel run //:image_to_card -- diskN --direct --image IOS27  that image, when the folder holds more than one

It does what install-ios.bin does on the Switch, in the same order, writing the container once at the card's raw
speed instead of copying the image onto the FAT for the Switch to read back:
  1 the partition's GPT entry becomes Linux "iOS-setup", so a half-written partition never boots or attaches
  2 the image's boot files into switchroot/ios/, bootloader/ini/ios.ini and its icon from out/sd
  3 the container, each 8 MiB chunk checked against the manifest before it is written, ZERO extents as zeros
  4 apfs_tool fits the container to the partition, measuring it before and after; fsck_apfs -n checks it
  5 the entry becomes APFS "iOS"; macOS must attach the container, and fsck_apfs checks it again
  6 the partition table is compared with the one from before and the boot files read back; the card is ejected
A rerun always installs again from step 1. Writes go only to the target partition (through its own device node,
each write bounds-checked), its GPT entry (both copies, never the hybrid MBR) and those FAT files; card.py's guards
choose and check the card.
"""

import argparse
import os
import sys
import time
import uuid
from pathlib import Path
from typing import NoReturn

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.common import files, paths, sudo
from tools.install import (
    card as cards,
    compose,
    direct_stream as streams,
    folder,
    gpt,
    host,
    manifest,
    progress,
    target as targets,
)

IOS = 'iOS'
SETUP = targets.SETUP_NAME
# macOS reads the partition table again when a whole disk opened for writing is closed; it takes well under a second.
REPROBE_SECONDS = 30
SETTLE_TRIES = 40
UNMOUNT_TRIES = 5
BLOCK = 4096
GB = 1e9


class Direct:
    def __init__(self, args: argparse.Namespace, log: host.Log) -> None:
        self.args, self.log = args, log
        self.name, self.m = image(Path(args.out), args.image)
        self.stream = streams.Stream(Path(args.out) / folder.SII_DIR, self.m)
        self.card: cards.Card | None = None
        self.slice, self.size = '', 0

    def close(self) -> None:
        self.stream.close()

    @property
    def target(self) -> gpt.Entry:
        return self.card.target

    def privileged(self) -> bool:
        return os.access(self.card.raw, os.R_OK | os.W_OK)

    def inspect(self) -> None:
        self.card = cards.resolve(self.args.disk)
        if not self.privileged():
            _, part = cards.check_layout_unprivileged(self.card, {}, self.args.partition)
            self.slice, self.size = part.identifier, part.size
        else:
            cards.read_table(self.card)
            cards.check_layout(self.card, {}, self.args.partition)
            self.slice, self.size = self.card.slice(self.target), self.target.bytes
            self.check_slice()
        if self.size < self.m.minimum_bytes:
            raise cards.RefusedError(
                f'{self.slice} has {self.size} bytes; the image needs {self.m.minimum_bytes} or more'
            )

    def check_slice(self) -> None:
        """Refuse unless the slice node is the target's GPT entry: the container is written through it."""
        info = host.disk_info(self.slice) or {}
        if (info.get('DiskUUID') or '').upper() != str(self.target.guid).upper() or (
            int(info.get('TotalSize', 0)) != self.target.bytes
        ):
            raise cards.RefusedError(f'{self.slice} is not GPT entry {self.target.describe()}')

    def plan(self) -> list[str]:
        m = self.m
        zeros = sum(n for _, s, n in m.extents if s == manifest.ZERO)
        fit = min(self.size, m.container_bytes)
        boot = ', '.join(b.name for b in m.boot)
        return [
            (
                f'image: {self.name} {m.label!r}: {m.write_bytes / GB:.2f} GB to write ({zeros / GB:.2f} GB '
                f'of it zeros, {len(m.extents)} extents), for a partition of {m.minimum_bytes / GB:.2f} GB or more'
            ),
            f'GPT entry -> Linux {SETUP!r} (macOS lets go of any container on {self.slice})',
            f'{cards.BOOT_ENTRY}/: {boot}; {cards.INI}, {cards.ICON}',
            f'write the container onto {self.slice}, each chunk checked against the manifest first',
            f'fit it to {fit // BLOCK} blocks with apfs_tool; fsck_apfs -n',
            f'GPT entry -> APFS {IOS!r}; macOS attaches the container; fsck_apfs -n',
            'compare the partition table, read the boot files back' + ('' if self.card.image else ', eject'),
        ]

    def run(self) -> int:
        boot = streams.boot_files(self.stream, self.m)
        self.inspect()
        where = f', image {self.card.image}' if self.card.image else ''
        self.log(f'card: {self.card.disk} ({self.card.size / GB:.1f} GB{where})')
        self.log('target:', self.target.describe() if self.target else f'{self.slice} ({self.size} bytes)')
        for line in self.plan():
            self.log('  plan:', line)
        if not self.args.yes:
            self.log('dry run: nothing written (add --yes)')
            return 0
        disk = self.card.disk
        cards.confirm(self.args, disk, f"This replaces {disk}'s iOS partition and everything on it.")
        if not self.privileged():
            if os.geteuid() == 0:
                raise host.HostError(f'{self.card.raw} cannot be opened for writing')
            self.close()
            reexec_as_root(self.args)
        before = self.backup()
        if not self.release():
            return 0
        self.boot_entry(boot)
        self.write()
        self.fit()
        self.activate()
        self.verify(before, boot)
        if not self.card.image and not self.args.no_eject:
            self.eject()
        self.log(f'done: {self.name} is installed on {self.slice}; choose iOS in hekate')
        return 0

    def backup(self) -> bytes:
        snapshot = cards.table_snapshot(self.card)
        path = Path(str(self.log.path) + '.partition-table.bin')
        path.write_bytes(snapshot)
        sudo.give_back(path)
        self.log('partition table backed up to', path)
        return snapshot

    def release(self) -> bool:
        """Make the target Linux "iOS-setup" with nothing attached on it; False when the card had to be ejected."""
        container = host.container_on(self.slice)
        if self.target.type == gpt.LINUX_DATA and self.target.name == SETUP and not container:
            return True
        self.unmount_all(container)
        self.retype(gpt.LINUX_DATA, SETUP)
        if self.reprobed(SETUP, attached=False):
            return True
        if not container:
            self.log(f'macOS has not reread the partition table yet; {self.slice} has nothing attached, going on')
            return True
        if self.card.image:
            raise host.HostError(f'macOS kept the container on {self.slice} attached after the retype')
        host.run('diskutil', 'eject', self.card.disk, log=self.log)
        self.log(
            f'{self.slice} is now {SETUP}, but macOS still had its old container attached. The card is ejected: '
            'reinsert it and run the same command again.'
        )
        return False

    def unmount_all(self, container: dict | None) -> None:
        """Nothing on the card may be mounted while its whole-disk node is open for writing (EBUSY otherwise).

        Spotlight and Finder hold a volume they just saw mounted for a moment, so a busy unmount is tried again.
        """
        disks = ([container['ContainerReference']] if container else []) + [self.card.disk]
        volumes = [v['DeviceIdentifier'] for v in (container or {}).get('Volumes', [])]
        for attempt in range(UNMOUNT_TRIES):
            for disk in disks:
                host.run('diskutil', 'unmountDisk', disk, log=self.log, check=attempt + 1 == UNMOUNT_TRIES)
            parts = [p['DeviceIdentifier'] for p in host.partitions(self.card.disk)]
            if not any(host.mount_point(identifier) for identifier in parts + volumes):
                return
            time.sleep(1)
        raise host.HostError(f'something on {self.card.disk} stays mounted: close what uses the card and run again')

    def retype(self, kind: uuid.UUID, name: str) -> None:
        index = self.target.index
        writes = gpt.edit(self.card.table, index, kind, name)
        try:
            fd = os.open(self.card.raw, os.O_RDWR)
        except OSError as error:
            raise host.HostError(f'{self.card.raw}: {error} (is something on the card still mounted?)') from None
        try:
            gpt.apply(fd, writes)
        finally:
            os.close(fd)
        cards.read_table(self.card)
        self.card.target = self.card.table.entries[index]
        if (self.target.type, self.target.name) != (kind, name):
            raise host.HostError(f'the retype did not take: {self.target.describe()}')
        self.log(f'GPT: {self.target.describe()}')

    def reprobed(self, name: str, attached: bool) -> bool:
        """Wait until macOS shows the slice by its new name, with a container on it or none (as `attached` says)."""
        deadline = time.monotonic() + REPROBE_SECONDS
        while time.monotonic() < deadline:
            info = host.disk_info(self.slice) or {}
            if info.get('IORegistryEntryName') == name and bool(host.container_on(self.slice)) == attached:
                return True
            time.sleep(0.5)
        return False

    def boot_entry(self, boot: list[tuple[str, bytes]]) -> None:
        out = Path(self.args.out)
        with cards.mounted(self.card.slice(self.card.fat), self.log) as root:
            for name, data in boot:
                replace(root / cards.BOOT_ENTRY / name, data)
            shared = [r for r in (cards.INI, cards.ICON) if (out / r).is_file()]
            for relative in shared:
                data = (out / relative).read_bytes()
                if not ((root / relative).is_file() and (root / relative).read_bytes() == data):
                    replace(root / relative, data)
            host.run('sync')
        self.log(f'FAT: {len(boot)} boot files into {cards.BOOT_ENTRY}/, {", ".join(shared)}')

    def write(self) -> None:
        self.check_slice()
        if host.container_on(self.slice):
            raise host.HostError(f'macOS has a container attached on {self.slice}: refusing to write under it')
        self.log(f'writing {self.m.write_bytes / GB:.2f} GB onto {self.slice}')
        start = time.monotonic()
        fd = os.open('/dev/r' + self.slice, os.O_RDWR)
        try:
            with progress.step(f'write: {self.slice}', self.m.write_bytes):
                written = streams.install(self.m, self.stream, fd, self.target.bytes, progress.advance)
        finally:
            os.close(fd)
        took = time.monotonic() - start
        self.log(f'write: {written} bytes in {took:.1f} s ({written / max(took, 1e-6) / (1 << 20):.1f} MiB/s)')

    def measure(self) -> tuple[int, int]:
        blocks, minimum, _, _ = map(int, host.run(self.args.apfs_tool, '/dev/r' + self.slice, '0').split())
        return blocks, minimum

    def fit(self) -> None:
        blocks, minimum = self.measure()
        want = min(self.target.bytes, self.m.container_bytes) // BLOCK
        if blocks * BLOCK != self.m.container_bytes or minimum > want:
            raise host.HostError(f'apfs_tool measures {blocks} blocks (fits down to {minimum}); expected the image')
        start = time.monotonic()
        host.run(self.args.apfs_tool, '/dev/r' + self.slice, '0', str(want), log=self.log)
        took = time.monotonic() - start
        if self.measure()[0] != want:
            raise host.HostError(f'apfs_tool did not fit the container on {self.slice} to {want} blocks')
        self.log(f'fit: {blocks} -> {want} blocks in {took:.1f} s (it fits down to {minimum})')
        compose.fsck(self.slice, 'fitted', self.log)

    def activate(self) -> None:
        self.unmount_all(None)
        self.retype(gpt.APFS, IOS)
        if not self.reprobed(IOS, attached=True):
            name = (host.disk_info(self.slice) or {}).get('IORegistryEntryName')
            if name == IOS:
                raise host.HostError(f'macOS did not attach the container on {self.slice} (fsck passed before)')
            self.log('macOS has not reread the partition table: reinsert the card to see the container attach')
            return
        container = host.container_on(self.slice)
        found = container.get('APFSContainerUUID', '').upper()
        if found != str(self.m.container_uuid).upper():
            raise host.HostError(f"the container on {self.slice} is {found}, not the image's")
        names = ', '.join(v.get('Name', '?') for v in container.get('Volumes', []))
        self.log(f'attached: {container["ContainerReference"]} ({names})')
        if self.unmount_container():
            compose.fsck(self.slice, 'attached', self.log)
        else:
            self.log('macOS keeps the new container mounted: the second fsck_apfs is skipped (the first passed)')

    def unmount_container(self) -> bool:
        """Unmount what macOS mounts of the container it attached, once it has stopped mounting for a second."""
        quiet = 0
        for _ in range(SETTLE_TRIES):
            container = host.container_on(self.slice) or {}
            if any(host.mount_point(v['DeviceIdentifier']) for v in container.get('Volumes', [])):
                host.run('diskutil', 'unmountDisk', container['ContainerReference'], log=self.log, check=False)
                quiet = 0
            elif (quiet := quiet + 1) == 2:
                return True
            time.sleep(0.5)
        return False

    def eject(self) -> None:
        try:
            host.run('diskutil', 'eject', self.card.disk, log=self.log)
            self.log('ejected', self.card.disk)
        except host.HostError as error:
            self.log(f'not ejected ({error}): eject {self.card.disk} in Finder before taking the card out')

    def verify(self, before: bytes, boot: list[tuple[str, bytes]]) -> None:
        cards.read_table(self.card)
        self.card.target = self.card.table.entries[self.target.index]
        changed = cards.changed_outside(self.card, before, cards.table_snapshot(self.card), self.target.index)
        apfs_entries = [e.index for e in self.card.table.used if e.type == gpt.APFS]
        if changed or apfs_entries != [self.target.index]:
            raise host.HostError(f'the partition table changed outside the target entry: offsets {changed[:16]}')
        records = {b.name: b for b in self.m.boot}
        with cards.mounted(self.card.slice(self.card.fat), self.log, read_only=True) as root:
            bad = [n for n, _ in boot if not streams.matches(read(root / cards.BOOT_ENTRY / n), records[n])]
        if bad:
            raise host.HostError(f'boot files read back wrong from the FAT: {", ".join(bad)}')
        self.log('verify: partition table changed only in the target entry (MBR untouched); boot files read back')


def image(out: Path, name: str | None) -> tuple[str, manifest.Manifest]:
    found = folder.images(out)
    names = [name] if name else sorted(found)
    if not names:
        raise host.HostError(f'{out} holds no image: run //:image first')
    if len(names) != 1:
        raise cards.RefusedError(f'{out} holds {", ".join(names)}: choose one with --image')
    m = found.get(names[0])
    if m is None:
        raise host.HostError(f'{out} holds no readable image {names[0]}: run //:image first')
    return names[0], m


def read(path: Path) -> bytes:
    return path.read_bytes() if path.is_file() else b''


def replace(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    files.write_bytes(path, data, sync=True)
    double = path.with_name('._' + path.name)
    if double.exists():
        double.unlink()


def resolve_tool(value: str | None) -> str:
    """Return apfs_tool's absolute path from its runfiles path (`bazel run`) or a path, so a sudo rerun finds it."""
    value = paths.located(value)
    if not value or not os.access(value, os.X_OK):
        raise host.HostError(f'no apfs_tool at {value!r}: run through `bazel run //:image_to_card`')
    return value


def reexec_as_root(args: argparse.Namespace) -> NoReturn:
    argv = [args.disk, '--yes']
    for key in ('confirm', 'out', 'image', 'partition', 'apfs_tool', 'log'):
        value = getattr(args, key)
        if value:
            argv.append(f'--{key.replace("_", "-")}={value}')
    if args.no_eject:
        argv.append('--no-eject')
    sudo.rerun(__file__, argv)


def parse(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('disk', help="the card's whole disk, e.g. disk4 (see `diskutil list external`)")
    parser.add_argument('--yes', action='store_true', help='install (default: dry run)')
    parser.add_argument('--confirm', help='the disk identifier, instead of typing it')
    parser.add_argument('--image', help='an image by name (IOS17, IOS27); needed when the folder holds several')
    parser.add_argument('--partition', help='the partition to install to, by GPT name, when the card has several')
    parser.add_argument('--out', help="//:image's folder (default: out/sd in the source tree)")
    parser.add_argument('--apfs-tool', default=os.environ.get('SWITCH_APFS_TOOL'))
    parser.add_argument('--no-eject', action='store_true')
    parser.add_argument('--log')
    args = parser.parse_args(argv)
    args.out = os.path.abspath(args.out or paths.workspace() / 'out/sd')
    args.log = os.path.abspath(args.log or paths.run_log('direct'))
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse(sys.argv[1:] if argv is None else argv)
    log = host.Log(args.log)
    direct = None
    try:
        host.check_tools(log)
        args.apfs_tool = resolve_tool(args.apfs_tool)
        direct = Direct(args, log)
        args.image = direct.name
        return direct.run()
    except cards.RefusedError as error:
        log('REFUSED:', error)
        return 2
    except (host.HostError, streams.StreamError, gpt.GptError, manifest.ManifestError) as error:
        log('FAILED:', error)
        return 1
    except KeyboardInterrupt:
        log(f'INTERRUPTED: run the same command again (a partition left {SETUP} does not boot until it finishes)')
        return 130
    finally:
        if direct:
            direct.close()
        log.quiet('log:', args.log)


if __name__ == '__main__':
    sys.exit(main())
