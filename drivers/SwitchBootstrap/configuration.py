# SPDX-License-Identifier: BSD-2-Clause-Patent
import argparse
import hashlib
import json
import re
from pathlib import Path

MANAGED = {
    'org.coolstar.sileo': 'sileo.deb',
    'ellekit': 'ellekit.deb',
    'uikittools': 'uikittools.deb',
    'launchctl': 'launchctl.deb',
}
DEVELOPMENT = {
    'openssh-client': 'openssh-client.deb',
    'openssh-sftp-server': 'openssh-sftp-server.deb',
    'openssh-server': 'openssh-server.deb',
    'switch-ssh': 'switch-ssh.deb',
    'top': 'top.deb',
}


def managed_names(records: dict) -> dict:
    """Return every image's four managed packages, followed by a development image's SSH and tool set."""
    if not isinstance(records, dict):
        raise ValueError('invalid managed package manifest')
    if set(records) == set(MANAGED):
        return MANAGED
    if set(records) == set(MANAGED) | set(DEVELOPMENT):
        return MANAGED | DEVELOPMENT
    raise ValueError('invalid managed package manifest')


def managed_packages(manifest: dict) -> dict:
    records = manifest.get('managed_packages')
    for name, filename in managed_names(records).items():
        record = records[name]
        if (
            not isinstance(record, dict)
            or set(record) != {'file', 'sha256', 'size', 'version'}
            or record['file'] != filename
            or not isinstance(record['sha256'], str)
            or not re.fullmatch(r'[0-9a-f]{64}', record['sha256'])
            or type(record['size']) is not int
            or not 0 < record['size'] <= 256 * 1024 * 1024
            or not isinstance(record['version'], str)
            or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.+:~_-]*', record['version'])
        ):
            raise ValueError('invalid managed package manifest')
    generation = hashlib.sha256(
        b'Switch bootstrap migration v1\n' + json.dumps(records, sort_keys=True, separators=(',', ':')).encode('ascii')
    ).hexdigest()
    if manifest.get('managed_generation') != generation:
        raise ValueError('invalid managed package generation')
    return records


def package_header(records: dict) -> str:
    entries = []
    for name in managed_names(records):
        fields = ', '.join(
            '@' + json.dumps(key) + ': @' + (str(value) if isinstance(value, int) else json.dumps(value))
            for key, value in records[name].items()
        )
        entries.append('@' + json.dumps(name) + ': @{' + fields + '}')
    return '#define SWITCH_BOOTSTRAP_MANAGED_PACKAGES @{' + ', '.join(entries) + '}\n'


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--helper')
    parser.add_argument('--build')
    parser.add_argument('--firmware')
    args = parser.parse_args()
    manifest = json.loads(Path(args.manifest).read_text())
    digest = manifest.get('archive_sha256', '')
    if manifest.get('schema_version') != 1 or not re.fullmatch(r'[0-9a-f]{64}', digest):
        raise ValueError('invalid bootstrap manifest')
    sileo_digest = manifest.get('sileo_executable_sha256')
    if not isinstance(sileo_digest, str) or not re.fullmatch(r'[0-9a-f]{64}', sileo_digest):
        raise ValueError('invalid Sileo executable digest')
    records = managed_packages(manifest)
    if not args.helper:
        text = (
            '#define SWITCH_BOOTSTRAP_ARCHIVE_SHA256 ' + json.dumps(digest) + '\n'
            '#define SWITCH_BOOTSTRAP_SILEO_SHA256 ' + json.dumps(sileo_digest) + '\n'
            '#define SWITCH_BOOTSTRAP_GENERATION '
            + json.dumps(manifest['managed_generation'])
            + '\n'
            + package_header(records)
        )
    else:
        if not re.fullmatch(r'[A-Za-z0-9]+', args.build or '') or not re.fullmatch(
            r'\d+(?:\.\d+)*', args.firmware or ''
        ):
            raise ValueError('invalid IPSW build or firmware')
        with Path(args.helper).open('rb') as helper:
            helper_digest = hashlib.file_digest(helper, 'sha256').hexdigest()
        manifest.update(
            helper_sha256=helper_digest,
            build=args.build,
            firmware=args.firmware,
        )
        text = json.dumps(manifest, sort_keys=True) + '\n'
    Path(args.out).write_text(text)


if __name__ == '__main__':
    main()
