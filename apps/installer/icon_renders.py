"""Freeze Icon Composer's device renderings; actool includes worker-specific pixels and rendition IDs."""

import argparse
import hashlib
import io
import json
import lzma
import plistlib
import tarfile
import zipfile
from pathlib import Path

ICONS = ('AppIcon60x60@2x.png', 'AppIcon76x76@2x~ipad.png', 'Assets.car')
ARCHIVE = 'icon-renders.tar.xz'
RECEIPT = 'icon-renders.json'


def source_hashes(icon: Path) -> dict[str, str]:
    return {
        p.relative_to(icon).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in icon.rglob('*')
        if p.is_file()
    }


def freeze(icon: Path, ipas: list[Path], out: Path) -> None:
    metadata = {}
    resources = {}
    for ipa in ipas:
        with zipfile.ZipFile(ipa) as package:
            app = 'Payload/SwitchInstaller.app/'
            info = plistlib.loads(package.read(app + 'Info.plist'))
            version = info['MinimumOSVersion']
            if info['DTPlatformName'] != 'iphoneos' or version in metadata:
                raise ValueError(f'expected distinct device builds: {ipa}')
            metadata[version] = {
                key: info[key] for key in ('DTXcodeBuild', 'DTSDKBuild', 'CFBundleIcons', 'CFBundleIcons~ipad')
            }
            for name in ICONS:
                resources[f'{version}/{name}'] = package.read(app + name)
    if set(metadata) != {'17.4', '27.0'}:
        raise ValueError('both Installer device targets are required')
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.USTAR_FORMAT) as archive:
        for name, data in sorted(resources.items()):
            member = tarfile.TarInfo(name)
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    content = lzma.compress(raw.getvalue())
    (out / ARCHIVE).write_bytes(content)
    receipt = {'archives': metadata, 'sha256': hashlib.sha256(content).hexdigest(), 'source': source_hashes(icon)}
    (out / RECEIPT).write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')


def apply(app: Path, icon: Path, inputs: Path) -> None:
    info = plistlib.loads((app / 'Info.plist').read_bytes())
    if info['DTPlatformName'] != 'iphoneos':
        return
    receipt = json.loads((inputs / RECEIPT).read_text())
    version = info['MinimumOSVersion']
    if source_hashes(icon) != receipt['source'] or any(
        info.get(k) != v for k, v in receipt['archives'][version].items()
    ):
        raise ValueError('Installer icon source or toolchain changed; refresh the pinned icon renderings')
    data = (inputs / ARCHIVE).read_bytes()
    if hashlib.sha256(data).hexdigest() != receipt['sha256']:
        raise ValueError('Installer icon archive does not match its receipt')
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:xz') as archive:
        for name in ICONS:
            source = archive.extractfile(f'{version}/{name}')
            if not source:
                raise ValueError(f'missing pinned Installer icon: {name}')
            (app / name).write_bytes(source.read())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--icon', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('ipas', nargs=2, type=Path)
    args = parser.parse_args()
    freeze(args.icon, args.ipas, args.out)
