#!/usr/bin/env python3
"""Make a stand-in for the Switch SD card: a sparse raw image file with the real card's partition table.

    testcard.py create <image> [--layout install/testdata/switch-card-1tb.json] [--variant normal|no-switch|
                       wrong-size|two-apfs]
    testcard.py attach <image>          print the /dev/diskN hdiutil gives it (attached -nomount, raw)
    testcard.py detach <image>

The image is the card's full size (1 TB) but sparse, so it costs only what is written: the partition table sectors
(the hybrid MBR, both GPT headers and entry arrays, copied from the recorded card), a formatted FAT32 hos_data
("SWITCH SD") with a few files of other boot entries the installer must leave alone, and later what the installer
writes. Variants break one thing each for the installer's refusal tests:
  no-switch   no hos_data FAT32 partition first (a generic GPT disk)
  wrong-size  userdata is one sector short of 32 GiB
  two-apfs    l4t (also 32 GiB) is APFS-typed too
"""

import argparse
import base64
import hashlib
import json
import plistlib
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.install import disks, gpt, host

DEFAULT_LAYOUT = Path(__file__).resolve().parents[2] / 'install/testdata/switch-card-1tb.json'
OTHER_FILES = {
    'bootloader/hekate_ipl.ini': b'[config]\nautoboot=0\n',
    'bootloader/ini/ubuntu.ini': b'[Ubuntu]\nl4t=1\nboot_prefixes=/switchroot/ubuntu/\n',
    'switchroot/ubuntu/boot.scr': b'not ours\n',
    'switchroot/ios/stale-old-file.bin': b'left by an older ios-on-switch-lmao build\n',
    'atmosphere/config/system_settings.ini': b'[eupld]\n',
    'payload.bin': b'\0' * 64,
}


def _sectors(layout: dict) -> dict[int, bytearray]:
    out = {}
    for record in layout['sectors']:
        data = base64.b64decode(record['base64'])
        if hashlib.sha256(data).hexdigest() != record['sha256'] or len(data) != 512 * record['count']:
            raise SystemExit('layout record at LBA {} is corrupt'.format(record['lba']))
        out[record['lba']] = bytearray(data)
    return out


def _recrc(sectors: dict[int, bytearray], disk_sectors: int) -> None:
    """Recompute both GPT headers' CRCs after an entry edit (variants only)."""
    entries_lba = [lba for lba in sectors if lba not in (0, 1, disk_sectors - 1)]
    primary_entries = bytes(sectors[min(entries_lba)])
    count, size = int.from_bytes(sectors[1][80:84], 'little'), int.from_bytes(sectors[1][84:88], 'little')
    crc = zlib.crc32(primary_entries[: count * size])
    sectors[max(entries_lba)][:] = primary_entries
    for lba in (1, disk_sectors - 1):
        header = sectors[lba]
        header[88:92] = crc.to_bytes(4, 'little')
        header[16:20] = b'\0\0\0\0'
        header[16:20] = zlib.crc32(bytes(header[:92])).to_bytes(4, 'little')


def create(image: str, layout_path: str, variant: str, filesystem: str = 'fat32') -> None:
    if filesystem not in ('fat32', 'exfat'):
        raise ValueError(f'unsupported filesystem {filesystem!r}')
    layout = json.loads(Path(layout_path).read_text())
    size = layout['disk_bytes']
    sectors = _sectors(layout)
    disk_sectors = size // 512
    entries = sectors[min(lba for lba in sectors if lba not in (0, 1, disk_sectors - 1))]
    if variant == 'wrong-size':
        at = 8 * 128  # userdata: last LBA one less
        last = int.from_bytes(entries[at + 40 : at + 48], 'little') - 1
        entries[at + 40 : at + 48] = last.to_bytes(8, 'little')
        _recrc(sectors, disk_sectors)
    elif variant == 'two-apfs':
        at = 1 * 128  # l4t
        entries[at : at + 16] = gpt.APFS.bytes_le
        _recrc(sectors, disk_sectors)
    elif variant == 'no-switch':
        at = 0  # hos_data becomes a Linux partition; the MBR loses its FAT32 entry
        entries[at : at + 16] = gpt.LINUX_DATA.bytes_le
        sectors[0][446 : 446 + 16] = bytes(16)
        _recrc(sectors, disk_sectors)
    if filesystem == 'exfat' and variant != 'no-switch':
        sectors[0][446 + 4] = 0x07
    image = Path(image)
    if image.exists():
        raise SystemExit(f'{image} exists')
    with open(image, 'wb') as f:
        f.truncate(size)
        for lba, data in sectors.items():
            f.seek(lba * 512)
            f.write(data)
    device = attach(image)
    try:
        if variant != 'no-switch':
            fat = device + 's1'
            formatter = ['/sbin/newfs_exfat'] if filesystem == 'exfat' else ['/sbin/newfs_msdos', '-F', '32']
            subprocess.run(
                [*formatter, '-v', 'SWITCH SD', '/dev/r' + fat[5:]],
                check=True,
                capture_output=True,
            )
            with tempfile.TemporaryDirectory() as work:
                log = host.Log(Path(work) / 'mount.log')
                try:
                    with disks.mounted(fat, log, owners=False) as mount:
                        for relative, data in OTHER_FILES.items():
                            path = mount / relative
                            path.parent.mkdir(parents=True, exist_ok=True)
                            path.write_bytes(data)
                finally:
                    log.file.close()
    finally:
        detach(image)
    print(f'{image}: {size} bytes, variant {variant}')


def _info() -> dict:
    return plistlib.loads(
        subprocess.run(['/usr/bin/hdiutil', 'info', '-plist'], check=True, capture_output=True).stdout
    )


def device_of(image: str | Path) -> str | None:
    image = str(Path(image).resolve())
    for item in _info().get('images', []):
        if str(Path(item.get('image-path', '')).resolve()) == image:
            disk = host.whole_disk_of(item.get('system-entities', []))
            if disk:
                return '/dev/' + disk
    return None


def attach(image: str | Path) -> str:
    current = device_of(image)
    if current:
        return current
    out = plistlib.loads(
        subprocess.run(
            [
                '/usr/bin/hdiutil',
                'attach',
                '-plist',
                '-nomount',
                '-imagekey',
                'diskimage-class=CRawDiskImage',
                str(image),
            ],
            check=True,
            capture_output=True,
        ).stdout
    )
    disk = host.whole_disk_of(out['system-entities'])
    if not disk:
        raise SystemExit(f'hdiutil attach gave no whole disk for {image}')
    return '/dev/' + disk


def detach(image: str | Path) -> None:
    device = device_of(image)
    if device:
        subprocess.run(['/usr/bin/hdiutil', 'detach', device], check=True, capture_output=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    c = sub.add_parser('create')
    c.add_argument('image')
    c.add_argument('--layout', default=str(DEFAULT_LAYOUT))
    c.add_argument('--filesystem', choices=['fat32', 'exfat'], default='fat32')
    c.add_argument('--variant', default='normal', choices=['normal', 'no-switch', 'wrong-size', 'two-apfs'])
    a = sub.add_parser('attach')
    a.add_argument('image')
    d = sub.add_parser('detach')
    d.add_argument('image')
    args = parser.parse_args()
    if args.command == 'create':
        create(args.image, args.layout, args.variant, args.filesystem)
    elif args.command == 'attach':
        print(attach(args.image))
    else:
        detach(args.image)


if __name__ == '__main__':
    main()
