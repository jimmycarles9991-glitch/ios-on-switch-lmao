import hashlib
import json
import plistlib
import re
import stat
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

from tools.common.hashing import sha256_file

APP = 'SwitchInstaller.app'
IDENTIFIER = 'com.brunostjohn.switch.installer'
MANIFEST = 'migration/installer-seed.json'
BUILTIN_MANIFEST = 'installer-bundle.json'


def capture(ipa: Path) -> dict:
    prefix = f'Payload/{APP}/'
    files = {}
    with zipfile.ZipFile(ipa) as archive:
        for item in archive.infolist():
            if not item.filename.startswith(prefix) or item.is_dir():
                continue
            name = item.filename[len(prefix) :]
            if '\\' in name or any(part in ('', '.', '..') for part in name.split('/')):
                raise ValueError(f'unsafe Installer bundle member: {name!r}')
            if name in files or stat.S_IFMT(item.external_attr >> 16) not in (0, stat.S_IFREG):
                raise ValueError(f'duplicate or non-file Installer bundle member: {name!r}')
            with archive.open(item) as stream:
                files[name] = hashlib.file_digest(stream, 'sha256').hexdigest()
        info = plistlib.loads(archive.read(prefix + 'Info.plist'))
    if info['CFBundleIdentifier'] != IDENTIFIER or info['CFBundleExecutable'] != 'SwitchInstaller':
        raise ValueError('unexpected Installer bundle identity')
    if 'SwitchInstaller' not in files or '_CodeSignature/CodeResources' not in files:
        raise ValueError('missing Installer executable or resource seal')
    return {'schema': 1, 'identifier': IDENTIFIER, 'version': info['CFBundleVersion'], 'files': files}


def verify(app: Path, pin: dict) -> None:
    if pin.get('schema') != 1 or pin.get('identifier') != IDENTIFIER or app.is_symlink() or not app.is_dir():
        raise ValueError(f'invalid Installer seed or manifest: {app}')
    wanted = pin['files']
    dirs = {parent for name in wanted for parent in Path(name).parents}
    seen = set()
    for p in app.rglob('*'):
        relative = p.relative_to(app)
        name = relative.as_posix()
        mode = p.lstat().st_mode
        if stat.S_ISDIR(mode) and relative in dirs:
            continue
        if not stat.S_ISREG(mode) or name not in wanted or sha256_file(p) != wanted[name]:
            raise ValueError(f'Installer seed differs from the image input: {name}')
        seen.add(name)
    if seen != set(wanted):
        raise ValueError(f'Installer seed missing files: {sorted(set(wanted) - seen)}')


def check_rights(ipa: Path, policy: Path, profile: str) -> None:
    with tempfile.TemporaryDirectory() as temp, zipfile.ZipFile(ipa) as archive:
        executable = Path(temp) / 'SwitchInstaller'
        executable.write_bytes(archive.read(f'Payload/{APP}/SwitchInstaller'))
        result = subprocess.run(
            ['/usr/bin/codesign', '-d', '--entitlements', '-', '--xml', executable], capture_output=True, check=True
        )
    rights = plistlib.loads(result.stdout)
    prefix, _, identifier = rights.get('application-identifier', '').partition('.')
    if identifier != IDENTIFIER or not re.fullmatch('[A-Z0-9]{10}', prefix):
        raise ValueError('Installer signing identity does not match the image policy')
    template = (
        policy.read_text().replace('$(AppIdentifierPrefix)', prefix + '.').replace('$(CFBundleIdentifier)', IDENTIFIER)
    )
    expected = plistlib.loads(template.encode())
    if profile == 'debug' and 'get-task-allow' in rights:
        expected['get-task-allow'] = True
    if 'com.apple.developer.team-identifier' in rights:
        expected['com.apple.developer.team-identifier'] = prefix
    if plistlib.dumps(expected) != plistlib.dumps(rights):
        raise ValueError('Installer entitlements differ from the image policy')


if __name__ == '__main__':
    ipa, out, policy = map(Path, sys.argv[1:4])
    check_rights(ipa, policy, sys.argv[4])
    out.write_text(json.dumps(capture(ipa), indent=2, sort_keys=True) + '\n')
