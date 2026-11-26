"""Extract the small pinned files the build reads from an IPSW's OS volume image (ipsw/os_files.bzl)."""

import argparse
import contextlib
import hashlib
import os
import plistlib
import re
import shutil
import subprocess
import tempfile
import time
from collections.abc import Iterator
from pathlib import Path

HDIUTIL = '/usr/bin/hdiutil'
DISKUTIL = '/usr/sbin/diskutil'
APFS_VOLUME = '41504653-0000-11AA-AA11-00306543ECAC'

SYSTEM_VERSION = 'System/Library/CoreServices/SystemVersion.plist'
INSTALLD = 'usr/libexec/installd'
LAUNCHD_CATALOG = 'System/Library/xpc/launchd.plist'
LAUNCHD_CATALOG_SIG = 'System/Library/xpc/launchd.plist.sig'
MIGRATION_WRAPPER = (
    'System/Library/PrivateFrameworks/DataMigration.framework/XPCServices/'
    'com.apple.migrationpluginwrapper.xpc/com.apple.migrationpluginwrapper'
)
MIGRATION_RUNNER = (
    'System/Library/PrivateFrameworks/DataMigration.framework/XPCServices/'
    'com.apple.datamigrator.xpc/com.apple.datamigrator'
)


def _plist(cmd: list[str]) -> dict:
    return plistlib.loads(subprocess.run(cmd, capture_output=True, check=True).stdout)


def _disk_info(device: str) -> dict:
    result = subprocess.run([DISKUTIL, 'info', '-plist', device], capture_output=True, check=False)
    return plistlib.loads(result.stdout) if result.returncode == 0 and result.stdout else {}


def _unmount(device: str) -> None:
    for _ in range(5):
        subprocess.run([DISKUTIL, 'unmount', device], capture_output=True, check=False)
        if not _disk_info(device).get('MountPoint'):
            return
        time.sleep(1)
    subprocess.run([DISKUTIL, 'unmount', 'force', device], capture_output=True, check=False)


def _detach(disk: str) -> None:
    result = subprocess.run([HDIUTIL, 'detach', disk], capture_output=True, check=False)
    if result.returncode:
        subprocess.run([HDIUTIL, 'detach', '-force', disk], capture_output=True, check=False)


@contextlib.contextmanager
def mounted_volume(image: Path, volume_uuid: str) -> Iterator[Path]:
    entities = _plist([HDIUTIL, 'attach', '-plist', '-readonly', '-nomount', '-noverify', '-noautofsck', str(image)])[
        'system-entities'
    ]
    disks = [e['dev-entry'][5:] for e in entities if re.fullmatch(r'/dev/disk\d+', e.get('dev-entry', ''))]
    if not disks:
        raise ValueError(f'hdiutil attach gave no disk for {image}')
    whole = disks[0]
    try:
        volumes = [
            e['dev-entry'][5:]
            for e in entities
            if e.get('content-hint', '').upper() == APFS_VOLUME and re.fullmatch(r'/dev/disk\d+s\d+', e['dev-entry'])
        ]
        matching = [v for v in volumes if _disk_info(v).get('VolumeUUID', '').upper() == volume_uuid.upper()]
        if len(matching) != 1:
            raise ValueError(f'selected IPSW System volume UUID {volume_uuid} not found')
        volume = matching[0]
        with tempfile.TemporaryDirectory(prefix='switch-os-files-') as mount_dir:
            for attempt in range(3):
                subprocess.run(
                    [
                        DISKUTIL,
                        'mount',
                        'readOnly',
                        '-mountOptions',
                        'nobrowse,noowners',
                        '-mountPoint',
                        mount_dir,
                        volume,
                    ],
                    capture_output=True,
                    check=attempt == 2,
                )
                where = _disk_info(volume).get('MountPoint')
                if where and os.path.realpath(where) == os.path.realpath(mount_dir):
                    break
                if where:
                    _unmount(volume)
            else:
                raise ValueError(f'{volume} would not mount at {mount_dir}')
            try:
                yield Path(mount_dir)
            finally:
                _unmount(volume)
    finally:
        _detach(whole)


def _copy_file(root: Path, out: Path, relative: str, mode: int) -> bytes:
    data = (root / relative).read_bytes()
    target = out / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    target.chmod(mode)
    return data


def extract(
    root: Path,
    build: str,
    out: Path,
    catalog_sha256: str = '',
    signature_sha256: str = '',
) -> None:
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    version = plistlib.loads(_copy_file(root, out, SYSTEM_VERSION, 0o644))
    if version.get('ProductBuildVersion') != build:
        raise ValueError(f'OS volume build {version.get("ProductBuildVersion")!r} differs from pinned {build!r}')
    _copy_file(root, out, INSTALLD, 0o755)
    if catalog_sha256 or signature_sha256:
        catalog = _copy_file(root, out, LAUNCHD_CATALOG, 0o644)
        signature = _copy_file(root, out, LAUNCHD_CATALOG_SIG, 0o644)
        if (
            hashlib.sha256(catalog).hexdigest() != catalog_sha256
            or hashlib.sha256(signature).hexdigest() != signature_sha256
        ):
            raise ValueError('native launchd catalogue or detached signature differs from the IPSW pins')
    migrators = root / 'System/Library/DataClassMigrators'
    ls_executable = None
    for plist_path in sorted(migrators.glob('*.migrator/Info.plist')):
        rel = str(plist_path.relative_to(root))
        info = plistlib.loads(_copy_file(root, out, rel, 0o644))
        if info.get('CFBundleIdentifier') == 'com.apple.-0LaunchServicesMigrator':
            ls_executable = str((plist_path.parent / info['CFBundleExecutable']).relative_to(root))
    if ls_executable:
        _copy_file(root, out, ls_executable, 0o755)
    for binary in (MIGRATION_WRAPPER, MIGRATION_RUNNER):
        if (root / binary).is_file():
            _copy_file(root, out, binary, 0o755)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--volume-uuid', required=True)
    parser.add_argument('--build', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--launchd-catalog-sha256', default='')
    parser.add_argument('--launchd-catalog-signature-sha256', default='')
    args = parser.parse_args()
    with mounted_volume(args.image, args.volume_uuid) as root:
        extract(root, args.build, args.out, args.launchd_catalog_sha256, args.launchd_catalog_signature_sha256)


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        detail = error.stderr or error.stdout
        raise SystemExit(
            detail.decode(errors='replace') if isinstance(detail, bytes) else detail or str(error)
        ) from None
    except (KeyError, OSError, ValueError) as error:
        raise SystemExit(str(error)) from None
