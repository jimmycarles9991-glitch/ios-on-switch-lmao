"""Stage a pinned rootless package tree separately from the replaceable system overlay."""

import json
import os
import posixpath
import re
import shutil
import tarfile
from dataclasses import dataclass
from pathlib import Path

from tools.common import hashing
from tools.install import bootstrap_migration, host, owners as ownership

DIRECTORY = 'Switch/bootstrap'
MANIFEST = 'manifest.json'
ARCHIVE = 'payload.tar'
HELPER = 'switch-bootstrap'
ROOT = 'root'
METADATA = 'metadata.json'
HELD = ('org.coolstar.sileo', 'ellekit', 'uikittools', 'launchctl')
DEVELOPMENT_HELD = ('openssh-client', 'openssh-sftp-server', 'openssh-server', 'switch-ssh', 'top')
BUILD = re.compile(r'[A-Za-z0-9]+')
SHA256 = re.compile(r'[0-9a-f]{64}')
MANAGED_ROOT = re.compile(re.escape(f'/private/var/{DIRECTORY}/') + r'[A-Za-z0-9]+/root')


def _valid_holds(holds: object) -> bool:
    if not isinstance(holds, list) or not all(isinstance(name, str) for name in holds):
        return False
    names = set(holds)
    return len(names) == len(holds) and names in (set(HELD), set(HELD) | set(DEVELOPMENT_HELD))


@dataclass(frozen=True)
class Payload:
    directory: Path
    manifest: dict

    @property
    def archive(self) -> Path:
        return self.directory / ARCHIVE

    @property
    def helper(self) -> Path:
        return self.directory.parent / 'overlay/usr/libexec' / HELPER


def payload(package: str | Path, pins: dict | None = None) -> Payload | None:
    directory = Path(package) / 'bootstrap'
    if not directory.exists():
        return None
    try:
        manifest = json.loads((directory / MANIFEST).read_text())
        valid = (
            manifest['schema_version'] == 1
            and SHA256.fullmatch(manifest['archive_sha256'])
            and SHA256.fullmatch(manifest['helper_sha256'])
            and _valid_holds(manifest.get('holds'))
            and (directory / ARCHIVE).is_file()
            and (Path(package) / 'overlay/usr/libexec' / HELPER).is_file()
        )
        if 'managed_generation' in manifest and (
            not isinstance(manifest['managed_generation'], str) or not SHA256.fullmatch(manifest['managed_generation'])
        ):
            raise ValueError('invalid managed package generation')
        if not valid:
            raise ValueError('invalid manifest or missing payload files')
        if pins:
            for field, value in (('build', pins['build']), ('firmware', pins['version'])):
                if field in manifest and manifest[field] != value:
                    raise ValueError(f'{field} differs from selected IPSW pins')
        return Payload(directory, manifest)
    except (OSError, KeyError, TypeError, ValueError) as error:
        raise host.HostError(f'bootstrap: {directory}: {error}') from error


def _name(name: str) -> str:
    while name.startswith('./'):
        name = name[2:]
    name = name.rstrip('/')
    if not name or name == '.':
        return ''
    if (
        name.startswith('/')
        or '\0' in name
        or len(name.encode()) >= 1024
        or posixpath.normpath(name) != name
        or '..' in name.split('/')
    ):
        raise host.HostError(f'bootstrap: unsafe archive path {name!r}')
    return name


def _members(archive: tarfile.TarFile) -> dict[str, tarfile.TarInfo]:
    members = {}
    for member in archive.getmembers():
        name = _name(member.name)
        if not name:
            if not member.isdir():
                raise host.HostError('bootstrap: archive root is not a directory')
            continue
        if name in members or not (member.isdir() or member.isfile() or member.issym() or member.islnk()):
            raise host.HostError(f'bootstrap: duplicate or unsupported archive member {name}')
        if not 0 <= member.uid <= 0x7FFFFFFF or not 0 <= member.gid <= 0x7FFFFFFF or member.mode & ~0o7777:
            raise host.HostError(f'bootstrap: invalid metadata for {name}')
        members[name] = member
    _links(members)
    return members


def _links(members: dict[str, tarfile.TarInfo]) -> None:
    for name, member in members.items():
        parent = posixpath.dirname(name)
        while parent:
            if parent in members and not members[parent].isdir():
                raise host.HostError(f'bootstrap: {name} is below non-directory {parent}')
            parent = posixpath.dirname(parent)
        if member.issym() and ('\0' in member.linkname or not member.linkname):
            raise host.HostError(f'bootstrap: invalid symlink {name}')
        if member.islnk():
            target = members.get(_name(member.linkname))
            if not target or not target.isfile():
                raise host.HostError(f'bootstrap: {name}: hard link must name a regular archive file')
            if (member.uid, member.gid, member.mode) != (target.uid, target.gid, target.mode):
                raise host.HostError(f'bootstrap: {name}: hard-link metadata differs')
    return members


def _metadata(members: dict[str, tarfile.TarInfo]) -> list[dict]:
    metadata = {'': {'path': '', 'uid': 0, 'gid': 0, 'mode': 0o755, 'kind': 'directory'}}
    for name, member in members.items():
        parent = posixpath.dirname(name)
        while parent:
            metadata.setdefault(parent, {'path': parent, 'uid': 0, 'gid': 0, 'mode': 0o755, 'kind': 'directory'})
            parent = posixpath.dirname(parent)
        metadata[name] = {
            'path': name,
            'uid': member.uid,
            'gid': member.gid,
            'mode': member.mode,
            'kind': 'directory' if member.isdir() else 'symlink' if member.issym() else 'file',
        }
    return sorted(metadata.values(), key=lambda item: item['path'])


def _copy_members(archive: tarfile.TarFile, root: Path, members: dict[str, tarfile.TarInfo]) -> None:
    for name, member in members.items():
        target = root / name
        if member.isfile():
            source = archive.extractfile(member)
            if source is None:
                raise host.HostError(f'bootstrap: cannot read {name}')
            with source, target.open('xb') as output:
                shutil.copyfileobj(source, output)
        elif member.issym():
            target.symlink_to(member.linkname)


def _extract(archive: tarfile.TarFile, root: Path) -> list[dict]:
    members = _members(archive)
    metadata = _metadata(members)
    for item in sorted(metadata, key=lambda item: item['path'].count('/')):
        if item['kind'] == 'directory':
            (root / item['path']).mkdir(parents=True, exist_ok=True)
    _copy_members(archive, root, members)
    for name, member in members.items():
        if member.islnk():
            os.link(root / _name(member.linkname), root / name, follow_symlinks=False)
    for item in metadata:
        if item['kind'] != 'symlink':
            (root / item['path']).chmod(item['mode'])
    return metadata


def _control(text: str) -> list[dict[str, str]]:
    records, record, key = [], {}, ''
    for line in [*text.splitlines(), '']:
        if not line:
            if record:
                records.append(record)
            record, key = {}, ''
        elif line[0].isspace() and key:
            record[key] += '\n' + line
        else:
            key, colon, value = line.partition(':')
            if not colon or not key or key in record:
                raise host.HostError('bootstrap: malformed dpkg status')
            record[key] = value.lstrip()
    return records


def _firmware(root: Path, version: str, holds: tuple[str, ...] | list[str] = HELD) -> None:
    status = root / 'Library/dpkg/status'
    if not status.is_file() or status.is_symlink():
        raise host.HostError('bootstrap: missing regular dpkg status database')
    records = _control(status.read_text())
    packages = {record.get('Package'): record for record in records}
    if (
        None in packages
        or '' in packages
        or len(packages) != len(records)
        or any(name not in packages for name in holds)
    ):
        raise host.HostError('bootstrap: dpkg database lacks protected core packages or lists duplicates')
    for name in holds:
        if packages[name].get('Status', '').split()[-2:] != ['ok', 'installed']:
            raise host.HostError(f'bootstrap: {name} is not configured')
        packages[name]['Status'] = 'hold ok installed'
    firmware = packages.get('firmware')
    if firmware is None:
        firmware = {'Package': 'firmware'}
        records.append(firmware)
    firmware.update(
        {
            'Status': 'hold ok installed',
            'Priority': 'required',
            'Section': 'System',
            'Essential': 'yes',
            'Architecture': 'iphoneos-arm64',
            'Version': version,
            'Description': 'iOS firmware version',
        }
    )
    status.write_text(
        '\n\n'.join('\n'.join(f'{key}: {value}' for key, value in record.items()) for record in records) + '\n'
    )


def _directory(path: Path) -> None:
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        raise host.HostError(f'bootstrap: {path} is not a real directory')
    path.mkdir(exist_ok=True)


def _link_target(data: Path, build: str) -> str:
    link = data / 'jb'
    target = f'/private/var/{DIRECTORY}/{build}/{ROOT}'
    if link.is_symlink():
        current = os.readlink(link)
        if current == target:
            return target
        if not MANAGED_ROOT.fullmatch(current):
            raise host.HostError('bootstrap: /var/jb points outside the managed bootstrap')
    elif link.exists():
        raise host.HostError('bootstrap: /var/jb already exists and is not a managed symlink')
    return target


def _activate(data: Path, build: str) -> None:
    target = _link_target(data, build)
    link = data / 'jb'
    if link.is_symlink() and os.readlink(link) == target:
        return
    partial = data / 'jb.partial'
    if partial.is_symlink() and MANAGED_ROOT.fullmatch(os.readlink(partial)):
        partial.unlink()
    if partial.exists() or partial.is_symlink():
        raise host.HostError('bootstrap: unexpected /var/jb.partial')
    partial.symlink_to(target)
    try:
        os.replace(partial, link)
    finally:
        partial.unlink(missing_ok=True)


def _remove_legacy_helper(target: Path) -> None:
    helper = target / HELPER
    if helper.exists() and not helper.is_file() and not helper.is_symlink():
        raise host.HostError('bootstrap: legacy helper path is not a file')
    helper.unlink(missing_ok=True)


def _existing(target: Path, build: str, version: str) -> dict:
    _directory(target)
    directory = os.open(target, os.O_DIRECTORY | os.O_RDONLY | os.O_NOFOLLOW)
    try:
        raw = bootstrap_migration.read_record(directory, MANIFEST, 256 * 1024, verify_owner=False)
        if raw is None:
            raise ValueError('missing manifest')
        installed = json.loads(raw)
    except (OSError, ValueError) as error:
        raise host.HostError(f'bootstrap: installed {build} has no valid manifest') from error
    finally:
        os.close(directory)
    if not isinstance(installed, dict):
        raise host.HostError('bootstrap: installed manifest is not an object')
    root = target / ROOT
    if (
        installed.get('build') != build
        or installed.get('firmware') != version
        or root.is_symlink()
        or not root.is_dir()
    ):
        raise host.HostError('bootstrap: installed tree does not match the selected IPSW')
    return installed


def _new(source: Payload, partial: Path, build: str, version: str) -> list[dict]:
    if partial.is_symlink() or (partial.exists() and not partial.is_dir()):
        raise host.HostError(f'bootstrap: {partial} is not a real staging directory')
    if partial.exists():
        shutil.rmtree(partial)
    partial.mkdir()
    try:
        root = partial / ROOT
        root.mkdir()
        with tarfile.open(source.archive, 'r:') as archive:
            metadata = _extract(archive, root)
        for marker in ('.procursus_strapped', '.switch-bootstrap-ready'):
            if not (root / marker).is_file() or (root / marker).is_symlink():
                raise host.HostError(f'bootstrap: payload lacks regular {marker}')
        _firmware(root, version, source.manifest['holds'])
        installed = dict(source.manifest, build=build, firmware=version)
        (partial / MANIFEST).write_text(json.dumps(installed, sort_keys=True) + '\n')
        (partial / METADATA).write_text(json.dumps(metadata, sort_keys=True) + '\n')
        return metadata
    except BaseException as error:
        if partial.exists():
            shutil.rmtree(partial)
        if isinstance(error, (OSError, tarfile.TarError, UnicodeError)):
            raise host.HostError(f'bootstrap: staging failed: {error}') from error
        raise


def _give_owners(data: Path, target: Path, metadata: list[dict], owners: ownership.Owners) -> None:
    for path in (data / 'Switch', data / DIRECTORY, target, target / MANIFEST, target / METADATA):
        owners.set(path, 0, 0, 0o755 if path.is_dir() else 0o644)
    for item in metadata:
        owners.set(target / ROOT / item['path'], item['uid'], item['gid'], item['mode'])


def stage(data: Path, package: str | Path, pins: dict, log: host.Log, owners: ownership.Owners | None = None) -> bool:
    source = payload(package, pins)
    if source is None:
        return False
    for path, field in ((source.archive, 'archive_sha256'), (source.helper, 'helper_sha256')):
        if hashing.sha256_file(path) != source.manifest[field]:
            raise host.HostError(f'bootstrap: {path.name} SHA-256 differs from its manifest')
    build, version = pins.get('build', ''), pins.get('version', '')
    if not BUILD.fullmatch(build) or not re.fullmatch(r'\d+(?:\.\d+)*', version):
        raise host.HostError('bootstrap: invalid IPSW build or firmware version')
    data = Path(data)
    _link_target(data, build)
    base = data / DIRECTORY
    for path in (data / 'Switch', base):
        _directory(path)
    target = base / build
    existed = target.exists() or target.is_symlink()
    if existed:
        installed = _existing(target, build, version)
        _remove_legacy_helper(target)
        _activate(data, build)
        changed = installed.get('archive_sha256') != source.manifest['archive_sha256']
        log(
            'bootstrap: existing packages kept'
            + ('; core payload changed, explicit package update required' if changed else '')
        )
    else:
        partial = base / (build + '.partial')
        metadata = _new(source, partial, build, version)
        if isinstance(owners, ownership.Immediate):
            _give_owners(data, partial, metadata, owners)
        partial.rename(target)
        if isinstance(owners, ownership.Journal):
            _give_owners(data, target, metadata, owners)
        _activate(data, build)
        if owners:
            owners.set(data / 'jb', 0, 0)
        log(f'bootstrap: staged {build} ({version}); adapted core package updates held')
    bootstrap_migration.schedule(data, build, source.manifest.get('managed_generation'), log, owners)
    return not existed


def unpacked_bytes(package: str | Path) -> int:
    source = payload(package)
    if source is None:
        return 0
    with tarfile.open(source.archive, 'r:') as archive:
        return sum((member.size + 4095) // 4096 * 4096 for member in _members(archive).values() if member.isfile())
