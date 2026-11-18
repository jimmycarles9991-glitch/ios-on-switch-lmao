import copy
import plistlib
import shutil
import struct
import subprocess
from pathlib import Path

from tools.common import macho
from tools.install_policy.installd import arm64_slice


def sign_file(path: Path, identifier: str | None = None, entitlements: Path | None = None) -> None:
    args = [
        '/usr/bin/codesign',
        '--force',
        '--sign',
        '-',
        '--timestamp=none',
        '--pagesize',
        '4096',
        '--preserve-metadata='
        + ('entitlements,flags,runtime' if identifier else 'identifier,entitlements,flags,runtime'),
        '--generate-entitlement-der',
    ]
    if identifier:
        args += ['--identifier', identifier]
    if entitlements:
        args += ['--entitlements', str(entitlements)]
    subprocess.run([*args, str(path)], check=True, capture_output=True)


def sign_bundle(source: Path, destination: Path, entitlements: Path) -> Path:
    shutil.copytree(source, destination, symlinks=True)
    for path in [destination, *destination.rglob('*')]:
        if not path.is_symlink():
            path.chmod(path.stat().st_mode | 0o200)
    images = []
    for path in destination.rglob('*'):
        if path.is_symlink() or not path.is_file():
            continue
        data = path.read_bytes()
        if data[:4] in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe'):
            path.write_bytes(arm64_slice(data))
            sign_file(path)
            images.append(path)
    bundles = [
        destination,
        *[p for p in destination.rglob('*') if p.suffix in ('.app', '.appex', '.framework', '.xpc')],
    ]
    for bundle in sorted(bundles, key=lambda p: len(p.parts), reverse=True):
        info = plistlib.loads((bundle / 'Info.plist').read_bytes())
        sign_file(bundle, info['CFBundleIdentifier'], entitlements if bundle == destination else None)
    for path in images:
        data = path.read_bytes()
        check_signature(data, macho.macho(data))
    info = plistlib.loads((destination / 'Info.plist').read_bytes())
    data = (destination / info['CFBundleExecutable']).read_bytes()
    check_signature(data, macho.macho(data), info['CFBundleIdentifier'])
    subprocess.run(
        ['/usr/bin/codesign', '--verify', '--deep', '--strict', str(destination)], check=True, capture_output=True
    )
    return destination


def sign(entries: dict, work: Path) -> None:
    target = work / 'image'
    for name, (member, data) in list(entries.items()):
        if not member.isfile() or data[:4] not in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe'):
            continue
        thin = arm64_slice(data)
        macho.macho(thin)
        target.write_bytes(thin)
        sign_file(target)
        subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(target)], check=True, capture_output=True)
        updated = target.read_bytes()
        image = macho.macho(updated)
        check_signature(updated, image)
        signed_member = copy.copy(member)
        signed_member.size = len(updated)
        entries[name] = signed_member, updated


def check_signature(data: bytes, image: dict, expected_identifier: str | None = None) -> None:
    commands = [offset for command, offset, _ in image['commands'] if command == macho.LC_CODE_SIGNATURE]
    if len(commands) != 1:
        raise ValueError('signed bootstrap image lacks one code signature')
    offset, length = struct.unpack_from('<II', data, commands[0] + 8)
    if offset + length > len(data) or length < 12:
        raise ValueError('code signature is outside the image')
    blob = data[offset : offset + length]
    magic, size, count = struct.unpack_from('>III', blob)
    if magic != 0xFADE0CC0 or size > len(blob) or 12 + 8 * count > size:
        raise ValueError('invalid signature superblob')
    slots, sha256 = set(), False
    for index in range(count):
        slot, start = struct.unpack_from('>II', blob, 12 + 8 * index)
        slots.add(slot)
        if start + 8 > size:
            raise ValueError('invalid signature slot')
        kind, extent = struct.unpack_from('>II', blob, start)
        if extent < 8 or start + extent > size:
            raise ValueError('invalid signature slot extent')
        if kind == 0xFADE0C02:
            if extent < 40 or blob[start + 39] != 12 or struct.unpack_from('>I', blob, start + 28)[0] == 0:
                raise ValueError('bootstrap CodeDirectory requires 4K code pages')
            sha256 |= blob[start + 37] in (2, 3)
            check_identifier(blob, start, extent, expected_identifier)
    if not sha256 or (5 in slots and 7 not in slots):
        raise ValueError('bootstrap signature requires SHA256 and DER entitlements')


def check_identifier(blob: bytes, start: int, extent: int, expected: str | None) -> None:
    if expected is None:
        return
    identifier = struct.unpack_from('>I', blob, start + 20)[0]
    if not 40 <= identifier < extent:
        raise ValueError('bootstrap CodeDirectory identifier offset is invalid')
    raw = blob[start + identifier : start + extent]
    if raw.split(b'\0', 1)[0] != expected.encode():
        raise ValueError('bootstrap CodeDirectory has the wrong privileged executable identity')
