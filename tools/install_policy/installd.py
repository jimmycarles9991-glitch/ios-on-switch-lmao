import argparse
import json
import os
import plistlib
import struct
import subprocess
import tempfile
from pathlib import Path

from tools.common import macho
from tools.install import disks, host

LIBRARY = '/usr/lib/libSwitchInstallPolicy.dylib'
SOURCE = 'usr/libexec/installd'
LC_LOAD_DYLIB = 0xC
DYLIB_COMMANDS = (LC_LOAD_DYLIB, 0x18 | 0x80000000, 0x1F | 0x80000000, 0x20, 0x23 | 0x80000000)
FAT_MAGICS = {
    b'\xca\xfe\xba\xbe': ('>', False),
    b'\xbe\xba\xfe\xca': ('<', False),
    b'\xca\xfe\xba\xbf': ('>', True),
    b'\xbf\xba\xfe\xca': ('<', True),
}


def arm64_slice(data: bytes) -> bytes:
    layout = FAT_MAGICS.get(data[:4])
    if layout is None:
        macho.macho(data, subtypes=None)
        return data
    endian, wide = layout
    if len(data) < 8:
        raise ValueError('truncated universal header')
    count = struct.unpack_from(endian + 'I', data, 4)[0]
    entry = struct.Struct(endian + ('IIQQII' if wide else 'IIIII'))
    end = 8 + count * entry.size
    if not count or end > len(data):
        raise ValueError('universal architecture table outside image')
    ranges, selected = [], []
    for index in range(count):
        cpu, subtype, offset, size, align, *_ = entry.unpack_from(data, 8 + index * entry.size)
        if align > 63 or offset % (1 << align) or offset < end or size < 32 or offset + size > len(data):
            raise ValueError('invalid universal slice extent or alignment')
        if any(offset < previous_end and previous_start < offset + size for previous_start, previous_end in ranges):
            raise ValueError('overlapping universal slices')
        ranges.append((offset, offset + size))
        if cpu == macho.CPU_TYPE_ARM64:
            image = data[offset : offset + size]
            parsed = macho.macho(image, subtypes=None)
            if parsed['cpusubtype'] != subtype:
                raise ValueError('universal and Mach-O CPU subtypes differ')
            selected.append(image)
    if len(selected) != 1:
        raise ValueError('expected exactly one ARM64 universal slice')
    return selected[0]


def dependencies(data: bytes, image: dict) -> list[int]:
    found = []
    for command, offset, length in image['commands']:
        if command not in DYLIB_COMMANDS:
            continue
        if length < 24:
            raise ValueError('truncated dylib command')
        name_at = struct.unpack_from('<I', data, offset + 8)[0]
        if not 24 <= name_at < length:
            raise ValueError('dylib name outside command')
        name = data[offset + name_at : offset + length]
        terminator = name.find(b'\0')
        if terminator < 0:
            raise ValueError('unterminated dylib name')
        if name[:terminator].decode() == LIBRARY:
            found.append(command)
    return found


def padding_end(data: bytes, image: dict, end: int) -> int:
    text = [s for s in image['segments'] if s['name'] == '__TEXT' and s['fileoff'] == 0]
    if len(text) != 1:
        raise ValueError('expected one file-backed __TEXT segment at offset zero')
    payload = []
    for segment in image['segments']:
        if segment['fileoff'] + segment['filesize'] > len(data):
            raise ValueError('segment outside image')
        if segment['fileoff'] and segment['filesize']:
            payload.append(segment['fileoff'])
        for section in segment['sections']:
            if not section['size'] or section['flags'] & 0xFF in (1, 0xC, 0x12):
                continue
            at, extent = section['fileoff'], section['size']
            if at < end or not segment['fileoff'] <= at <= at + extent <= segment['fileoff'] + segment['filesize']:
                raise ValueError('file-backed section outside segment or overlapping load commands')
            payload.append(at)
    if not payload:
        raise ValueError('installd has no file-backed sections')
    return min(*payload, text[0]['filesize'])


def inject(data: bytes) -> bytes:
    data = arm64_slice(data)
    image = macho.macho(data, subtypes=None)
    if image['kind'] != macho.MH_EXECUTE or image['cpusubtype'] & 0xFFFFFF not in (0, 2):
        raise ValueError('expected an ARM64 or ARM64e executable')
    count, size = struct.unpack_from('<II', data, 16)
    end = 32 + size
    limit = padding_end(data, image, end)
    found = dependencies(data, image)
    if found:
        if found != [LC_LOAD_DYLIB]:
            raise ValueError('policy library must have exactly one required dependency')
        return data
    name = LIBRARY.encode() + b'\0'
    length = (24 + len(name) + 7) & ~7
    if end + length > limit:
        raise ValueError('installd has insufficient load-command padding')
    if any(data[end : end + length]):
        raise ValueError('installd load-command padding is not empty')
    command = struct.pack('<6I', LC_LOAD_DYLIB, length, 24, 0, 0, 0) + name
    patched = bytearray(data)
    patched[end : end + length] = command.ljust(length, b'\0')
    struct.pack_into('<II', patched, 16, count + 1, size + length)
    macho.macho(patched, subtypes=None)
    return bytes(patched)


def entitlements(path: Path) -> dict:
    result = subprocess.run(
        ['/usr/bin/codesign', '--display', '--entitlements', ':-', str(path)],
        capture_output=True,
        check=True,
    )
    value = plistlib.loads(result.stdout)
    if not isinstance(value, dict) or not value:
        raise ValueError('installd has no entitlement dictionary')
    return value


def prepare(root: Path, pins: dict, out: Path) -> None:
    build = plistlib.loads((root / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if build.get('ProductBuildVersion') != pins['build']:
        raise ValueError('OS volume build differs from the selected IPSW pins')
    source = root / SOURCE
    original = entitlements(source)
    data = inject(source.read_bytes())
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='switch-install-policy-', dir=out.parent) as scratch:
        target, rights = Path(scratch) / 'installd', Path(scratch) / 'entitlements.plist'
        target.write_bytes(data)
        target.chmod(0o755)
        rights.write_bytes(plistlib.dumps(original))
        subprocess.run(
            [
                '/usr/bin/codesign',
                '--force',
                '--sign',
                '-',
                '--timestamp=none',
                '--preserve-metadata=identifier,flags,runtime',
                '--entitlements',
                str(rights),
                '--generate-entitlement-der',
                str(target),
            ],
            check=True,
            capture_output=True,
        )
        subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(target)], check=True, capture_output=True)
        if entitlements(target) != original:
            raise ValueError('re-signing changed installd entitlements')
        if inject(target.read_bytes()) != target.read_bytes():
            raise ValueError('re-signing removed the required install-policy dependency')
        os.replace(target, out)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--os', required=True, type=Path)
    parser.add_argument('--pins', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    pins = json.loads(args.pins.read_text())
    with tempfile.TemporaryDirectory(prefix='switch-install-policy-') as scratch:
        log = host.Log(Path(scratch) / 'mount.log')
        try:
            with disks.interrupts(), disks.attached(args.os, log, writable=False) as image:
                volumes = [
                    v
                    for v in image.volumes
                    if (host.disk_info(v) or {}).get('VolumeUUID', '').upper()
                    == pins['volumes']['os_volume_uuid'].upper()
                ]
                if len(volumes) != 1:
                    raise ValueError('selected IPSW System volume UUID not found')
                with disks.mounted(volumes[0], log, owners=False, read_only=True) as root:
                    prepare(root, pins, args.out)
        finally:
            log.file.close()


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit((error.stderr or b'').decode(errors='replace') or str(error)) from None
    except (ValueError, OSError, host.HostError) as error:
        raise SystemExit(str(error)) from None
