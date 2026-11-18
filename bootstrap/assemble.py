import argparse
import hashlib
import io
import json
import posixpath
import struct
import tarfile
import tempfile
from pathlib import Path

from bootstrap import packages
from bootstrap.launchctl.package import adapt_launchctl
from bootstrap.signing import sign, sign_bundle
from tools.common import macho
from tools.install_policy.installd import DYLIB_COMMANDS

SILEO_VERSION = '2.5.1+switch5'
ELLEKIT_VERSION = '1:0+git1017a0d.switch2'


def metadata(text: str, fields: dict) -> bytes:
    lines = text.strip().splitlines()
    for name, value in fields.items():
        matches = [i for i, line in enumerate(lines) if line.startswith(name + ':')]
        if len(matches) > 1:
            raise ValueError('duplicate control field')
        if matches:
            lines[matches[0]] = f'{name}: {value}'
        else:
            lines.append(f'{name}: {value}')
    return ('\n'.join(lines) + '\n').encode()


def read_control(deb: Path) -> dict:
    members = packages.read_deb(deb)
    control = [data for name, data in members.items() if name.startswith('control.tar')]
    if len(control) != 1:
        raise ValueError('Debian package needs one control archive')
    with tarfile.open(fileobj=io.BytesIO(control[0])) as archive:
        return packages.read_tar(archive, strip_prefix=False)


def adapt_sileo_control(control: dict) -> None:
    for name in ('postinst', 'prerm'):
        data = control[name][1].decode()
        data = data.replace('#!/bin/sh', '#!/var/jb/bin/sh')
        data = data.replace('sourcesDir="/etc/apt/', 'sourcesDir="/var/jb/etc/apt/')
        data = data.replace('/usr/lib/libblackjack.dylib', '/var/jb/usr/lib/libblackjack.dylib')
        data = data.replace('uicache -p /Applications/', '/var/jb/usr/bin/uicache -p /var/jb/Applications/')
        data = data.replace(
            'if [ -z "${SILEO}" ]; then /var/jb/usr/bin/uicache',
            'if [ "${SWITCH_BOOTSTRAP_UPDATE:-0}" != 1 ] && [ -z "${SILEO}" ]; then /var/jb/usr/bin/uicache',
        )
        packages.add_file(control, name, data.encode(), 0o755)


def adapt_prep(entries: dict) -> None:
    member, data = entries['prep_bootstrap.sh']
    shebang = b'#!/var/jb/bin/sh\n'
    if not data.startswith(shebang):
        raise ValueError('pinned bootstrap preparation interpreter changed')
    packages.add_file(entries, 'prep_bootstrap.sh', shebang + b'set -e\n' + data[len(shebang) :], member.mode)


def package_sidefiles(entries: dict, package: str) -> dict:
    prefix = f'Library/dpkg/info/{package}.'
    return {
        name.removeprefix(prefix): entry
        for name, entry in entries.items()
        if name.startswith(prefix) and name.removeprefix(prefix) not in ('list', 'md5sums', 'control')
    }


def tree(root: Path, destination: str) -> dict:
    entries = {}
    for path in sorted(root.rglob('*')):
        name = destination + '/' + path.relative_to(root).as_posix()
        if path.is_symlink():
            packages.add_link(entries, name, str(path.readlink()))
        elif path.is_file():
            packages.add_file(entries, name, path.read_bytes(), path.stat().st_mode & 0o777)
        elif path.is_dir():
            member = tarfile.TarInfo(name)
            member.type, member.mode = tarfile.DIRTYPE, 0o755
            entries[name] = member, b''
    return entries


def command_text(data: bytes, offset: int, length: int) -> str:
    cursor = struct.unpack_from('<I', data, offset + 8)[0]
    if not 12 <= cursor < length:
        raise ValueError('Mach-O path is outside its load command')
    raw = data[offset + cursor : offset + length]
    if b'\0' not in raw:
        raise ValueError('unterminated Mach-O path')
    return raw.split(b'\0', 1)[0].decode()


def rootless_library_exists(entries: dict, path: str) -> bool:
    if not path.startswith('/var/jb/'):
        return False
    relative = path.removeprefix('/var/jb/')
    for _ in range(32):
        entry = entries.get(relative)
        if entry is None:
            return False
        if entry[0].isfile():
            return True
        if not entry[0].issym():
            return False
        target = entry[0].linkname
        relative = (
            target.removeprefix('/var/jb/')
            if target.startswith('/var/jb/')
            else posixpath.normpath(posixpath.join(posixpath.dirname(relative), target))
        )
    raise ValueError('cyclic bootstrap library symlink')


def dependency_present(entries: dict, dependency: str, rpaths: list[str]) -> bool:
    if dependency in ('/usr/lib/libellekit.dylib', '/usr/local/lib/libellekit.dylib'):
        return False
    if dependency.startswith(('/System/Library/', '/usr/lib/')):
        return True
    if dependency.startswith('@rpath/'):
        return any(rootless_library_exists(entries, path + '/' + dependency.removeprefix('@rpath/')) for path in rpaths)
    return rootless_library_exists(entries, dependency)


def validate_linkage(entries: dict) -> None:
    critical = {
        'Applications/Sileo.app/Sileo': None,
        'usr/lib/libellekit.dylib': '/var/jb/usr/lib/libellekit.dylib',
        'usr/lib/ellekit/libinjector.dylib': '/var/jb/usr/lib/ellekit/libinjector.dylib',
        'usr/lib/ellekit/MobileSafety.dylib': '/var/jb/usr/lib/ellekit/MobileSafety.dylib',
    }
    for name, install_name in critical.items():
        data = entries[name][1]
        image = macho.macho(data)
        paths = [
            (command, command_text(data, offset, length))
            for command, offset, length in image['commands']
            if command in (*DYLIB_COMMANDS, 0xD, 0x8000001C)
        ]
        if install_name and [path for command, path in paths if command == 0xD] != [install_name]:
            raise ValueError(f'{name}: wrong rootless install name')
        rpaths = [path for command, path in paths if command == 0x8000001C]
        for command, dependency in paths:
            if command in DYLIB_COMMANDS and not dependency_present(entries, dependency, rpaths):
                raise ValueError(f'{name}: unresolved bootstrap dependency {dependency}')


def control_package(package: str, version: str, text: str = '') -> dict:
    result = {}
    packages.add_file(
        result,
        'control',
        metadata(
            text,
            {
                'Package': package,
                'Version': version,
                'Architecture': 'iphoneos-arm64',
                'Maintainer': 'Switch',
                'Description': 'Switch bootstrap integration',
            },
        ),
    )
    return result


def assemble(args: argparse.Namespace) -> None:
    provenance = base_provenance(args)
    entries = packages.read_bootstrap(args.base, args.zstd_lib)
    adapt_prep(entries)
    for required in ('Library/dpkg/status', 'bin/sh', '.procursus_strapped'):
        if required not in entries:
            raise ValueError('incomplete Procursus bootstrap')
    with tempfile.TemporaryDirectory(prefix='switch-bootstrap-sign-', dir=args.out.parent) as temporary:
        work = Path(temporary)
        sign(entries, work)
        args.debs.mkdir(parents=True, exist_ok=True)
        adapt_uikittools(entries, args.debs / 'uikittools.deb', args.uicache, args.sbreload)
        adapt_launchctl(entries, args.debs / 'launchctl.deb', args.launchctl, args.launchctl_license)
        app = sign_bundle(args.sileo_built / 'Sileo.app', work / 'Sileo.app', args.sileo_built / 'Sileo.entitlements')
        sileo = tree(app, 'var/jb/Applications/Sileo.app')
        packages.add_file(
            sileo, 'var/jb/usr/share/doc/Sileo/LICENSE', (args.sileo_built / 'Sileo.LICENSE').read_bytes()
        )
        sileo.update(tree(args.sileo_built / 'licenses', 'var/jb/usr/share/doc/Sileo/dependencies'))
        control = read_control(args.sileo_deb)
        packages.add_file(control, 'control', metadata(control['control'][1].decode(), {'Version': SILEO_VERSION}))
        adapt_sileo_control(control)
        packages.register_package(entries, control, sileo)
        packages.write_deb(args.debs / 'sileo.deb', control, sileo)
        ellekit = {}
        for name, destination in (
            ('libellekit.dylib', 'usr/lib/libellekit.dylib'),
            ('libinjector.dylib', 'usr/lib/ellekit/libinjector.dylib'),
            ('MobileSafety.dylib', 'usr/lib/ellekit/MobileSafety.dylib'),
        ):
            packages.add_file(ellekit, 'var/jb/' + destination, (args.built / name).read_bytes())
        for name, target in (
            ('usr/lib/TweakLoader.dylib', '/var/jb/usr/lib/ellekit/libinjector.dylib'),
            ('usr/lib/TweakInject.dylib', '/var/jb/usr/lib/ellekit/libinjector.dylib'),
            ('usr/lib/libsubstrate.dylib', '/var/jb/usr/lib/libellekit.dylib'),
            ('usr/lib/libhooker.dylib', '/var/jb/usr/lib/libellekit.dylib'),
            ('usr/lib/libblackjack.dylib', '/var/jb/usr/lib/libellekit.dylib'),
            ('Library/MobileSubstrate/DynamicLibraries', '/var/jb/usr/lib/TweakInject'),
            ('Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate', '/var/jb/usr/lib/libellekit.dylib'),
        ):
            packages.add_link(ellekit, 'var/jb/' + name, target)
        for name in ('usr/lib/TweakInject', 'Library/LaunchDaemons'):
            directory = tarfile.TarInfo('var/jb/' + name)
            directory.type, directory.mode = tarfile.DIRTYPE, 0o755
            ellekit[directory.name] = directory, b''
        for name in ('ElleKit.LICENSE', 'ElleKit.Logger.swift'):
            packages.add_file(ellekit, 'var/jb/usr/share/doc/ellekit/' + name, (args.built / name).read_bytes())
        control = control_package(
            'ellekit',
            ELLEKIT_VERSION,
            'Provides: mobilesubstrate (= 99), org.coolstar.libhooker (= 1.6.9)\n'
            'Conflicts: com.ex.substitute, org.coolstar.libhooker, science.xnu.substitute, '
            'mobilesubstrate, com.saurik.substrate.safemode\n'
            'Replaces: com.ex.libsubstitute, org.coolstar.libhooker, mobilesubstrate\n',
        )
        packages.add_file(
            control,
            'postinst',
            b'#!/var/jb/bin/sh\n'
            b'set -- ${SILEO:-${CYDIA:-}}\n'
            b'if [ "$#" -eq 2 ] && [ "$2" = 1 ]; then\n'
            b'  case "$1" in ""|*[!0-9]*) exit 0;; esac\n'
            b'  echo finish:usreboot >&"$1"\nfi\n',
            0o755,
        )
        sign(ellekit, work)
        packages.register_package(entries, control, ellekit)
        packages.write_deb(args.debs / 'ellekit.deb', control, ellekit)
    for name in ('sileo.deb', 'ellekit.deb', 'uikittools.deb', 'launchctl.deb'):
        packages.add_file(entries, 'usr/share/Switch/bootstrap-packages/' + name, (args.debs / name).read_bytes())
    packages.add_file(entries, '.switch_strapped', b'1\n')
    packages.add_file(entries, '.switch-bootstrap-ready', b'1\n')
    refresh_checksums(entries)
    packages.ensure_directories(entries)
    validate_linkage(entries)
    args.out.write_bytes(packages.tar_bytes(entries))
    managed = managed_package_manifest(args.debs)
    generation = hashlib.sha256(
        b'Switch bootstrap migration v1\n' + json.dumps(managed, sort_keys=True, separators=(',', ':')).encode('ascii')
    ).hexdigest()
    details = {
        'schema_version': 1,
        'archive_sha256': hashlib.sha256(args.out.read_bytes()).hexdigest(),
        'sileo_executable_sha256': hashlib.sha256(entries['Applications/Sileo.app/Sileo'][1]).hexdigest(),
        'managed_packages': managed,
        'managed_generation': generation,
        'holds': ['org.coolstar.sileo', 'ellekit', 'uikittools', 'launchctl'],
        'license_notices': {'depictionkit': 'Pinned upstream README lists its license as TBD; LICENSE.md is absent.'},
        'sources': json.loads(args.sources.read_text()),
    }
    details['base'] = provenance
    args.manifest.write_text(json.dumps(details, sort_keys=True, indent=2) + '\n')


def managed_package_manifest(directory: Path) -> dict:
    result = {}
    for package, filename in (
        ('org.coolstar.sileo', 'sileo.deb'),
        ('ellekit', 'ellekit.deb'),
        ('uikittools', 'uikittools.deb'),
        ('launchctl', 'launchctl.deb'),
    ):
        path = directory / filename
        text = read_control(path)['control'][1].decode()
        names = [line[9:] for line in text.splitlines() if line.startswith('Package: ')]
        versions = [line[9:] for line in text.splitlines() if line.startswith('Version: ')]
        if names != [package] or len(versions) != 1 or not versions[0]:
            raise ValueError(f'{filename}: invalid managed package identity')
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        result[package] = {'file': filename, 'sha256': digest, 'size': path.stat().st_size, 'version': versions[0]}
    return result


def base_provenance(args: argparse.Namespace) -> dict:
    digest = hashlib.sha256(args.base.read_bytes()).hexdigest()
    pin = next(item for item in json.loads(args.sources.read_text())['files'] if item['name'] == 'procursus_bootstrap')
    if digest != pin['sha256']:
        raise ValueError('Procursus bootstrap does not match the pinned release')
    return {'kind': 'official-prebuilt', 'archive_sha256': digest, 'url': pin['url'], 'cfver': '1900'}


def refresh_checksums(entries: dict) -> None:
    for name, (_, data) in list(entries.items()):
        if not name.startswith('Library/dpkg/info/') or not name.endswith('.md5sums'):
            continue
        lines = []
        for line in data.decode().splitlines():
            checksum, path = line.split(maxsplit=1)
            relative = path.removeprefix('/').removeprefix('./').removeprefix('var/jb/')
            if relative in entries and entries[relative][0].isfile():
                checksum = hashlib.md5(entries[relative][1]).hexdigest()
            lines.append(checksum + '  ' + path)
        packages.add_file(entries, name, ('\n'.join(lines) + '\n').encode())


def adapt_uikittools(entries: dict, out: Path, uicache: Path, sbreload: Path) -> None:
    package = 'uikittools'
    status = entries['Library/dpkg/status'][1].decode()
    stanza = next(s for s in status.split('\n\n') if s.startswith('Package: uikittools\n'))
    control = package_sidefiles(entries, package)
    text = '\n'.join(line for line in stanza.splitlines() if not line.startswith('Status:')) + '\n'
    version = next(line[9:] for line in text.splitlines() if line.startswith('Version: '))
    packages.add_file(control, 'control', metadata(text, {'Version': version + '+switch4'}))
    files = {}
    for line in entries['Library/dpkg/info/uikittools.list'][1].decode().splitlines():
        name = line.partition(' -> ')[0]
        if name in ('/.', '/var', '/var/jb'):
            continue
        relative = name.removeprefix('/var/jb/').removeprefix('var/jb/')
        if relative not in entries:
            raise ValueError(f'pinned uikittools file is missing: {name}')
        files['var/jb/' + relative] = entries[relative]
    packages.add_file(files, 'var/jb/usr/bin/uicache', uicache.read_bytes(), 0o755)
    packages.add_file(files, 'var/jb/usr/bin/sbreload', sbreload.read_bytes(), 0o755)
    packages.register_package(entries, control, files)
    packages.write_deb(out, control, files)


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in (
        'base',
        'sileo-deb',
        'sileo-built',
        'built',
        'uicache',
        'sbreload',
        'launchctl',
        'launchctl-license',
        'out',
        'manifest',
        'debs',
        'sources',
        'zstd-lib',
    ):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    for name, path in vars(args).items():
        setattr(args, name, path.resolve())
    assemble(args)


if __name__ == '__main__':
    main()
