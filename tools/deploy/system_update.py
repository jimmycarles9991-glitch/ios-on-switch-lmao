#!/usr/bin/env python3
"""Replace the installed card's translated System cryptex with this build's, keeping Data. No sudo needed.

    bazel run //deploy:system --config=ios27 -- <diskN>                         dry run: what it would write
    bazel run //deploy:system --config=ios27 -- <diskN> --yes --confirm=<diskN>  write it, then eject

iOS 27's SystemOS cryptex (the DriverKit dyld cache among its translated files) is one sealed image file on Preboot,
which the kernel attaches by its path at every boot and APFS checks block by block against the image's own seal. So
this build's image (//install:translated_system_cryptex) replaces that one file whole, through a partial file renamed
over it. Nothing else on Preboot changes, and Data is never mounted.

The kernel trusts the image's translated binaries through the boot package's trust module and /chosen list, both
pinned (ipsw/pretranslated/<kind>). So the tool refuses unless the translation matches those pins and the card's
boot entry (what //deploy:sd writes on the FAT: switchroot/ios/bl31.bin, bl33.bin, bootloader/ini/ios.ini) is this
build's, byte for byte. It also refuses iOS 17, whose cryptexes are Apple's own images, unchanged since the install.
//deploy:sd plans the same write in its check and makes it just before it writes the boot entry, so a deploy never
leaves the card's cryptex behind its boot package; this tool is for the cryptex alone.

Beside the image goes its record (tools/install/cryptex_record.py), which the install and every write keep: the image
counts as this build's only while its record says so. Without a record that still names the file, the image is written,
never read back to compare.
"""

import argparse
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.common import files, hashing, paths
from tools.deploy import deploy, pull_logs
from tools.install import card as cards, cryptex_record, host, progress, renamed

ROLE = 'system_cryptex'
# What the boot package carries of the System cryptex's translation (pretranslate.KEPT).
TRUSTED = (ROLE + '.trustcache', 'a64x-cache-pretranslated.bin')
# The old image stays until the new one is renamed over it: room for both, and some to spare.
SPARE_BYTES = 1 << 30


@dataclass
class Image:
    """This build's translated System cryptex and its sha256 (the .sha256 Bazel writes beside it)."""

    path: Path
    sha256: str


@dataclass
class Update:
    """The one write: the staged image on Preboot, what the card holds there now, and whether its record names it."""

    relative: str
    size: int
    card_size: int
    same: bool


def check_translation(trusted: list[Path], pins: dict[str, str]) -> None:
    """Refuse a translation whose trust module or cache list is not the one the boot package was built with."""
    given = {path.name: path for path in trusted}
    wrong = [name for name in TRUSTED if name not in given or hashing.sha256_file(given[name]) != pins.get(name)]
    if wrong:
        raise cards.RefusedError(
            'this build translated the System cryptex differently from the pins the boot package carries: {}'.format(
                ', '.join(wrong)
            )
        )


def check_boot_entry(fat_root: Path, package: Path, log: host.Log) -> None:
    """Refuse a card whose boot entry is not this build's: its trust modules would not cover the new image."""
    changed, _ = cards.sync_boot_entry(fat_root, package, log, dry_run=True)
    if changed:
        raise cards.RefusedError(
            "the card's boot entry is not this build's ({}): deploy this build first "
            '(bazel run //deploy:sd --config=ios27 -- <diskN> --yes)'.format(', '.join(changed))
        )


def plan(preboot_root: Path, spec: dict, image: Image, free_bytes: int, folder: str | None = None) -> Update:
    """Decide the write: the System cryptex's staged path, and whether the card's record names this image.

    `folder` is the image's folder on the card while a rename to the spec's is still to come (renamed.migrate_preboot).
    A partial image a stopped write left beside it counts as free, by the blocks it holds (the copy sizes it first, so
    a stopped one is mostly holes): the write replaces it first.
    """
    path = next(c['path'] for c in spec['cryptexes'] if c['member'] == ROLE)
    relative = path[len(renamed.PREBOOT) :]
    if Path(relative).name != image.path.name:
        raise cards.RefusedError(f'the kernel opens {path}, this build made {image.path.name}')
    current = Path(preboot_root) / (str(Path(folder) / image.path.name) if folder else relative)
    if not current.is_file():
        raise cards.RefusedError(f'Preboot has no {path}: install the image first')
    size, card_size = image.path.stat().st_size, current.stat().st_size
    same = cryptex_record.read(current) == image.sha256
    room = free_bytes + allocated(files.partial(current))
    if not same and room < size + SPARE_BYTES:
        raise cards.RefusedError(
            f'the card has {progress.size(room)} free, the new image needs {progress.size(size + SPARE_BYTES)} beside '
            'the old one until it replaces it'
        )
    return Update(relative, size, card_size, same)


def allocated(path: Path) -> int:
    """Return the bytes a file holds on its volume (0: none there): a sparse file's holes hold none."""
    try:
        return path.stat().st_blocks * 512
    except FileNotFoundError:
        return 0


def described(update: Update) -> str:
    state = 'unchanged' if update.same else 'to replace'
    return f'preboot: {update.relative} {state} (this build {update.size} bytes, the card {update.card_size} bytes)'


def write(preboot_root: Path, update: Update, image: Image, log: host.Log) -> None:
    """Copy the image over the staged one: a partial file in the same folder, renamed over it once whole."""
    target = Path(preboot_root) / update.relative
    with (
        files.replacing(target, sync=True) as partial,
        progress.step(f'preboot: writing {image.path.name}', update.size),
    ):
        host.copy_file_parallel(image.path, partial)
        partial.chmod(0o644)
    cryptex_record.write(target, image.sha256)
    log(f'preboot: {update.relative} replaced ({update.size} bytes)')


def planned_on_card(card: cards.Card, spec: dict, image: Image, target: object, log: host.Log) -> Update:
    """//deploy:sd's check: the System cryptex's write planned with Preboot read-only, so it refuses before writing."""
    preboot = pull_logs.data_volume(card, spec['volumes']['preboot']['uuid'])
    if not preboot:
        raise cards.RefusedError(f'{card.disk}: no iPad Preboot volume, so the System cryptex cannot follow this build')
    with cards.mounted(preboot, log, read_only=True) as preboot_root:
        moved = renamed.migrate_preboot(preboot_root, spec, log, dry_run=True)
        update = plan(preboot_root, spec, image, free_bytes(target), folder=moved[0] if moved else None)
    log(described(update))
    return update


def write_on_card(preboot: str, update: Update, image: Image, target: object, log: host.Log) -> None:
    """//deploy:sd's write of what planned_on_card decided, once the room is still there.

    Data shares the container, and the deploy's Data writes come between the plan and this, so the room is measured
    again here, before anything of the image is written.
    """
    if update.same:
        return
    with cards.mounted(preboot, log) as preboot_root:
        room = free_bytes(target) + allocated(files.partial(Path(preboot_root) / update.relative))
        if room < update.size + SPARE_BYTES:
            raise host.HostError(
                f'the card has {progress.size(room)} free after the Data writes, the new System cryptex needs '
                f'{progress.size(update.size + SPARE_BYTES)}: neither it nor the boot entry was written'
            )
        write(preboot_root, update, image, log)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('disk')
    parser.add_argument('--yes', action='store_true')
    parser.add_argument('--confirm')
    parser.add_argument('--no-eject', action='store_true')
    parser.add_argument('--boot-package', required=True)
    parser.add_argument('--spec', required=True)
    parser.add_argument('--ipsw', required=True, help="the IPSW kind's pins (//install:ipsw_pins)")
    parser.add_argument('--pretranslated-pins', help='{file name: sha256} the boot package carries (iOS 27)')
    parser.add_argument('--image', help='//install:translated_system_cryptex (iOS 27)')
    parser.add_argument('--trusted', action='append', default=[], help=' and '.join(TRUSTED) + ' (iOS 27)')
    parser.add_argument('--log')
    return parser


def _inputs(args: argparse.Namespace) -> tuple[Image, Path, dict]:
    """Return the image, its trusted files and the pretranslate pins; refuse a kind with no translated system."""
    ipsw = json.loads(Path(args.ipsw).read_text())
    if not (args.image and args.trusted and args.pretranslated_pins):
        raise cards.RefusedError(
            "iPadOS {} ({}) has no translated system: its cryptexes are Apple's own images, unchanged since the "
            'install, so there is nothing to update'.format(ipsw.get('version'), ipsw.get('build'))
        )
    image = Image(Path(args.image), Path(args.image + '.sha256').read_text().strip())
    return image, [Path(path) for path in args.trusted], json.loads(Path(args.pretranslated_pins).read_text())


def _checked_card(args: argparse.Namespace, spec: dict) -> tuple[cards.Card, object, object, str]:
    card = cards.resolve(args.disk)
    fat, target = cards.check_layout_unprivileged(card, spec)
    if target.content != 'Apple_APFS':
        raise cards.RefusedError(f'{card.disk}: {target.identifier} is not installed yet')
    deploy.check_kind(card, args.ipsw)
    preboot = pull_logs.data_volume(card, spec['volumes']['preboot']['uuid'])
    if not preboot:
        raise cards.RefusedError(f'{card.disk}: no iPad Preboot volume {spec["volumes"]["preboot"]["uuid"]}')
    return card, fat, target, preboot


def free_bytes(target: object) -> int:
    container = host.container_on(target.identifier) or {}
    return int(container.get('CapacityFree', 0))


def _eject(card: cards.Card, target: object, log: host.Log) -> None:
    container = host.container_on(target.identifier)
    if container:
        host.run('diskutil', 'unmountDisk', container['ContainerReference'], log=log, check=False)
    host.run('diskutil', 'eject', card.disk, log=log)
    log('ejected', card.disk)


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    for name in ('boot_package', 'spec', 'ipsw', 'pretranslated_pins', 'image'):
        setattr(args, name, paths.located(getattr(args, name)))
    args.trusted = [paths.located(path) for path in args.trusted]
    log = host.Log(args.log or paths.run_log('system', 'switch-ios-install'))
    try:
        image, trusted, pins = _inputs(args)
        check_translation(trusted, pins)
        host.check_tools(log)
        spec = pull_logs.load_spec(args.spec)
        card, fat, target, preboot = _checked_card(args, spec)
        with cards.mounted(fat.identifier, log, read_only=True) as fat_root:
            check_boot_entry(fat_root, Path(args.boot_package), log)
        with cards.mounted(preboot, log, read_only=not args.yes) as preboot_root:
            update = plan(preboot_root, spec, image, free_bytes(target))
            log(described(update))
            if update.same:
                log("nothing to write: the card holds this build's System cryptex")
            elif not args.yes:
                log(f'dry run: would write {update.size} bytes to Preboot {preboot} (add --yes)')
                return 0
            else:
                cards.confirm(args, card.disk, f'Replace the System cryptex on {card.disk}.')
                write(preboot_root, update, image, log)
        if args.yes and not args.no_eject:
            _eject(card, target, log)
        return 0
    except cards.RefusedError as error:
        log('REFUSED:', error)
        return 2
    except host.HostError as error:
        log('FAILED:', error)
        return 1


if __name__ == '__main__':
    sys.exit(main())
