from pathlib import Path

from bootstrap import packages

VERSION = '1:1.2.0+switch2'


def adapt_launchctl(entries: dict, output: Path, launchctl: Path, license_file: Path) -> None:
    status = entries['Library/dpkg/status'][1].decode()
    (stanza,) = (s for s in status.split('\n\n') if s.startswith('Package: launchctl\n'))
    metadata = [line for line in stanza.splitlines() if not line.startswith(('Status:', 'Version:'))]
    control = {}
    packages.add_file(control, 'control', ('\n'.join([*metadata, f'Version: {VERSION}']) + '\n').encode())
    files = {}
    for line in entries['Library/dpkg/info/launchctl.list'][1].decode().splitlines():
        name = line.partition(' -> ')[0]
        if name in ('/.', '/var', '/var/jb'):
            continue
        if not name.startswith('/var/jb/'):
            raise ValueError(f'invalid Procursus launchctl package path: {name}')
        files[name[1:]] = entries[name.removeprefix('/var/jb/')]
    packages.add_file(files, 'var/jb/usr/bin/launchctl', launchctl.read_bytes(), 0o755)
    packages.add_file(files, 'var/jb/usr/share/doc/launchctl/LICENSE', license_file.read_bytes())
    packages.register_package(entries, control, files)
    packages.write_deb(output, control, files)
