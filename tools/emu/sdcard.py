"""The emulated microSD card: 2 TiB, hybrid MBR, GPT, hekate's FAT and the iOS APFS partition, composed without copies.

QEMU's SD card must be a power of two, so the card is 2 TiB (SDXC's largest) rather than the user's 1 TB. The card is
a VMDK descriptor of flat extents: the partition tables, the FAT, and the iOS partition mapped straight onto the
install image (tools/install/manifest.py: its copy extents are the container's bytes, uncompressed) with a sparse
file of zeros between them. QEMU opens it with snapshot=on, so a run writes to a temporary overlay and never to the
shared base. The base (tables, an empty FAT, APFS clones of the image's parts) is made once per image (cache.py,
keyed by the manifest, which hashes every chunk of the image); each run gets its own clone of the FAT with its boot
files.
"""

import hashlib
import json
import shutil
import struct
import subprocess
import uuid
import zlib
from pathlib import Path

from tools.emu import cache
from tools.install import manifest

SECTOR = 512
CARD_SECTORS = (2 << 40) // SECTOR
IOS_FIRST = 2048
NO_IMAGE_SECTORS = (32 << 30) // SECTOR
FAT_SECTORS = (256 << 20) // SECTOR
ENTRIES = 128
ENTRY_BYTES = 128
ENTRY_SECTORS = ENTRIES * ENTRY_BYTES // SECTOR
TAIL_SECTORS = ENTRY_SECTORS + 1
APFS_TYPE = uuid.UUID('7C3457EF-0000-11AA-AA11-00306543ECAC')
BASIC_DATA = uuid.UUID('EBD0A0A2-B9E5-4433-87C0-68B6B72699C7')
# The same card on every run, as the real one is.
DISK_GUID = uuid.UUID('5357494B-0000-4000-8000-000000000001')
FAT_GUID = uuid.UUID('5357494B-0000-4000-8000-000000000002')
IOS_GUID = uuid.UUID('5357494B-0000-4000-8000-000000000003')
LAYOUT_REVISION = 2


def _entry(kind: uuid.UUID, unique: uuid.UUID, first: int, last: int, name: str) -> bytes:
    name_bytes = name.encode('utf-16le').ljust(72, b'\0')
    return kind.bytes_le + unique.bytes_le + struct.pack('<QQQ', first, last, 0) + name_bytes


def _header(current: int, backup: int, entries_lba: int, array_crc: int, last_usable: int) -> bytes:
    header = bytearray(
        struct.pack(
            '<8sIIII4Q16sQIII', b'EFI PART', 0x10000, 92, 0, 0, current, backup, 34, last_usable, DISK_GUID.bytes_le,
            entries_lba, ENTRIES, ENTRY_BYTES, array_crc,
        )
    )  # fmt: skip
    struct.pack_into('<I', header, 16, zlib.crc32(header))
    return bytes(header).ljust(SECTOR, b'\0')


def _mbr(fat_first: int) -> bytes:
    """Return hekate's hybrid MBR: its FAT32 as entry 1, the rest of the card behind a protective 0xEE entry."""
    mbr = bytearray(SECTOR)
    struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\xfe\xff\xff', 0x0C, b'\xfe\xff\xff', fat_first, FAT_SECTORS)
    struct.pack_into('<B3sB3sII', mbr, 462, 0, b'\0\2\0', 0xEE, b'\xfe\xff\xff', 1, IOS_FIRST - 1)
    mbr[510:512] = b'\x55\xaa'
    return bytes(mbr)


def layout(ios_sectors: int) -> dict:
    ios_last = IOS_FIRST + ios_sectors - 1
    fat_first = (ios_last + 1 + 2047) // 2048 * 2048
    return {'ios': [IOS_FIRST, ios_last], 'fat': [fat_first, fat_first + FAT_SECTORS - 1]}


def tables(plan: dict) -> tuple[bytes, bytes]:
    """Return the card's first IOS_FIRST sectors (MBR, primary GPT) and its last TAIL_SECTORS (backup GPT)."""
    array = _entry(BASIC_DATA, FAT_GUID, *plan['fat'], 'hos_data') + _entry(APFS_TYPE, IOS_GUID, *plan['ios'], 'iOS')
    array = array.ljust(ENTRIES * ENTRY_BYTES, b'\0')
    crc = zlib.crc32(array)
    last = CARD_SECTORS - 1
    last_usable = last - TAIL_SECTORS
    head = _mbr(plan['fat'][0]) + _header(1, last, 2, crc, last_usable) + array
    tail = array + _header(last, 1, last - ENTRY_SECTORS, crc, last_usable)
    return head.ljust(IOS_FIRST * SECTOR, b'\0'), tail


def image_manifest(first_part: Path) -> manifest.Manifest:
    with first_part.open('rb') as f:
        return manifest.decode(f.read(manifest.BOOT_OFFSET))


def container_extents(image: manifest.Manifest, parts: list[str]) -> list[list]:
    """Return the iOS partition as [sectors, file name, sector offset] runs (None: zeros) from the image's extents."""
    starts, at = [], 0
    for part in image.parts:
        starts.append(at)
        at += part.bytes
    runs, cursor = [], 0
    for offset, source, length in image.extents:
        if offset > cursor:
            runs.append([(offset - cursor) // SECTOR, None, 0])
        cursor = offset + length
        if source == manifest.ZERO:
            runs.append([length // SECTOR, None, 0])
            continue
        stream, remaining = manifest.BODY_OFFSET + source, length
        while remaining:
            index = max(i for i, s in enumerate(starts) if s <= stream)
            take = min(remaining, starts[index] + image.parts[index].bytes - stream)
            runs.append([take // SECTOR, parts[index], (stream - starts[index]) // SECTOR])
            stream, remaining = stream + take, remaining - take
    if image.container_bytes > cursor:
        runs.append([(image.container_bytes - cursor) // SECTOR, None, 0])
    return runs


def _mtools(fat: Path, *args: str) -> None:
    if not shutil.which(args[0]):
        raise SystemExit('the emulated SD card needs mtools: brew install mtools')
    subprocess.run([args[0], '-i', str(fat), *args[1:]], check=True, capture_output=True)


def raw_container(image: Path) -> tuple[int, int]:
    """Return a raw stage image's container (tools/install compose/populate) as its first sector and sector count."""
    offset = json.loads(image.with_name('state.json').read_text())['offset']
    with image.open('rb') as f:
        f.seek(offset + 32)
        magic, block_size, block_count = struct.unpack('<4sIQ', f.read(16))
    if magic != b'NXSB':
        raise SystemExit(f'{image}: no APFS container at {offset}')
    return offset // SECTOR, block_size * block_count // SECTOR


def is_install_image(source: Path) -> bool:
    return source.name.endswith('.SII.001')


def _make_base(tmp: Path, first_part: Path | None) -> None:
    if first_part is None:
        runs, ios_sectors = [[NO_IMAGE_SECTORS, None, 0]], NO_IMAGE_SECTORS
    elif not is_install_image(first_part):
        start, ios_sectors = raw_container(first_part)
        subprocess.run(['/bin/cp', '-c', str(first_part), str(tmp / first_part.name)], check=True)
        runs = [[ios_sectors, first_part.name, start]]
    else:
        image = image_manifest(first_part)
        parts = [Path(part.path).name for part in image.parts]
        for name in parts:
            subprocess.run(['/bin/cp', '-c', str(first_part.parent / name), str(tmp / name)], check=True)
        runs, ios_sectors = container_extents(image, parts), image.container_bytes // SECTOR
    plan = layout(ios_sectors)
    head, tail = tables(plan)
    (tmp / 'head.img').write_bytes(head)
    (tmp / 'tail.img').write_bytes(tail)
    with (tmp / 'zero.img').open('wb') as f:
        f.truncate(CARD_SECTORS * SECTOR)
    fat = tmp / 'fat.img'
    with fat.open('wb') as f:
        f.truncate(FAT_SECTORS * SECTOR)
    _mtools(fat, 'mformat', '-F', '-T', str(FAT_SECTORS), '-h', '255', '-s', '63', '-H', str(plan['fat'][0]),
            '-v', 'SWITCH SD', '::')  # fmt: skip
    (tmp / 'layout.json').write_text(json.dumps({'plan': plan, 'ios': runs}))


def base(first_part: Path | None) -> Path:
    """Return the shared card base for an install image (first part), a raw stage image or none; made once."""
    identity = {}
    if first_part is not None and is_install_image(first_part):
        with first_part.open('rb') as f:
            identity = {'manifest': hashlib.sha256(f.read(manifest.BOOT_OFFSET)).hexdigest()}
    elif first_part is not None:
        stat = first_part.stat()
        state = json.loads(first_part.with_name('state.json').read_text())
        identity = {'image': first_part.name, 'state': state, 'size': stat.st_size, 'mtime': stat.st_mtime_ns}
    key = cache.digest({'image': identity, 'revision': LAYOUT_REVISION})
    return cache.entry('sd-base', key, lambda tmp, _: _make_base(tmp, first_part))


def copy_files(fat: Path, files: dict[str, Path]) -> None:
    """Copy files onto the FAT (card path -> host file), making their directories."""
    made = set()
    for card in sorted(files):
        parts = card.split('/')[:-1]
        for i in range(1, len(parts) + 1):
            directory = '/'.join(parts[:i])
            if directory not in made:
                _mtools(fat, 'mmd', '-D', 's', '::/' + directory)
                made.add(directory)
        _mtools(fat, 'mcopy', '-o', str(files[card]), '::/' + card)


def _descriptor(entry: Path, fat: Path) -> str:
    saved = json.loads((entry / 'layout.json').read_text())
    plan, zero = saved['plan'], entry / 'zero.img'
    runs = [[IOS_FIRST, 'head.img', 0], *saved['ios']]
    runs.append([plan['fat'][0] - plan['ios'][1] - 1, None, 0])
    runs.append([FAT_SECTORS, fat, 0])
    runs.append([CARD_SECTORS - TAIL_SECTORS - plan['fat'][1] - 1, None, 0])
    runs.append([TAIL_SECTORS, 'tail.img', 0])
    if sum(run[0] for run in runs) != CARD_SECTORS:
        raise SystemExit('sdcard: the card descriptor does not add up to the card')
    lines = [f'RW {n} FLAT "{entry / name if name else zero}" {offset}' for n, name, offset in runs if n]
    header = ['# Disk DescriptorFile', 'version=1', 'CID=fffffffe', 'parentCID=ffffffff', 'createType="monolithicFlat"']
    # No blank line before an extent: QEMU's sscanf skips the newline and reads the next line's extent twice.
    return '\n'.join([*header, '# Extent description', *lines]) + '\n'


def card_for_run(first_part: Path | None, run: Path, files: dict[str, Path]) -> Path:
    """Return the run's card: a VMDK over the shared base and the run's own FAT clone with its boot files."""
    entry = base(first_part)
    run.mkdir(parents=True, exist_ok=True)
    fat = run / 'card-fat.img'
    fat.unlink(missing_ok=True)
    subprocess.run(['/bin/cp', '-c', str(entry / 'fat.img'), str(fat)], check=True)
    copy_files(fat, files)
    card = run / 'card.vmdk'
    card.write_text(_descriptor(entry, fat))
    return card
