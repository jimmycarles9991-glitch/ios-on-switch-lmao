#!/usr/bin/env python3
"""Put //:image's folder (out/sd) on a Switch SD card in the Mac: its images, install-ios.bin, the hekate ini and icon.

    bazel run //:image_to_card -- diskN                  dry run: what would be written and removed
    bazel run //:image_to_card -- diskN --yes            do it, then eject the card
    bazel run //:image_to_card -- diskN --image IOS27    only that image (default: every image in the folder)
    bazel run //:image_to_card -- diskN --direct [--yes] the image straight onto the iOS partition (direct.py)

Only parts the card does not already hold are written, by the manifests' chunk hashes: after a kernel-only change that
is the head of NAME.SII.001. The boot files hekate starts (switchroot/ios/bl31.bin, ...) are never written here;
install-ios.bin makes an image's own active when it installs it.
"""

import argparse
import os
import shutil
import sys
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.common import paths
from tools.install import card as cards, direct, folder, host, progress


def fat_of(card: cards.Card) -> cards.Part:
    """Return the card's hos_data FAT32, the only partition the copy touches; any iOS target is the installer's."""
    parts = cards.diskutil_partitions(card)
    if not parts or parts[0].name != 'hos_data' or parts[0].content != 'Microsoft Basic Data':
        raise cards.RefusedError(f"{card.disk}: the first partition is not the Switch's hos_data FAT32")
    return parts[0]


def stale_bytes(root: Path, relatives: list[str]) -> int:
    return sum((root / r).stat().st_size for r in relatives if (root / r).is_file())


def plan(out: Path, root: Path, names: list[str], log: host.Log) -> int:
    """Log what copying `names` and the shared files would do; return the bytes the FAT needs free for it."""
    need = 0
    for name in names:
        copies, stale = folder.copy_plan(out, root, name)
        for relative, start, end in copies:
            log(f'  copy: {relative}' + (' (head only)' if end - start < (out / relative).stat().st_size else ''))
        for relative in stale:
            log(f'  remove: {relative}')
        need += sum(end - start for _, start, end in copies) - stale_bytes(root, stale)
        if not copies and not stale:
            log(f'  {name}.SII: already on the card')
    for relative in folder.shared_plan(out, root):
        log(f'  copy: {relative}')
        need += (out / relative).stat().st_size - stale_bytes(root, [relative])
    return need


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('disk', help="the card's whole disk, e.g. disk4 (see `diskutil list external`)")
    parser.add_argument('--yes', action='store_true', help='write the plan (without it: a dry run)')
    parser.add_argument('--image', action='append', help='an image by name (IOS17, IOS27); default: all of them')
    parser.add_argument('--out', help="//:image's folder (default: out/sd in the source tree)")
    parser.add_argument('--no-eject', action='store_true')
    parser.add_argument('--log')
    args = parser.parse_args(argv)
    out = Path(os.path.abspath(args.out or paths.workspace() / 'out/sd'))
    log = host.Log(args.log or paths.run_log('to-card'))
    try:
        found = folder.images(out)
        names = args.image or sorted(found)
        broken = [n for n in names if found.get(n) is None]
        if not names or broken:
            raise host.HostError(f'{out} holds no readable image {" ".join(broken)}: run //:image first')
        card = cards.resolve(args.disk)
        fat = fat_of(card)
        with cards.mounted(fat.identifier, log, read_only=not args.yes) as root:
            need = plan(out, Path(root), names, log)
            free = shutil.disk_usage(root).free
            if need > free:
                raise cards.RefusedError(
                    f"the card's FAT has {free >> 20} MiB free; the images need {need >> 20} MiB more"
                )
            if not args.yes:
                log('dry run: nothing written (add --yes)')
                return 0
            folder.merge(out, Path(root), names, log)
        if not args.no_eject:
            with progress.step(f'card: ejecting {card.disk}', unit=None):
                host.run('diskutil', 'eject', card.disk, log=log)
            log('ejected', card.disk, "- in the Switch, run install-ios.bin from hekate's Payloads")
        return 0
    except cards.RefusedError as error:
        log('REFUSED:', error)
        return 2
    except host.HostError as error:
        log('FAILED:', error)
        return 1


if __name__ == '__main__':
    if '--direct' in sys.argv[1:]:
        sys.exit(direct.main([a for a in sys.argv[1:] if a != '--direct']))
    sys.exit(main())
