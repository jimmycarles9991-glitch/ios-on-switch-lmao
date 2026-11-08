#!/usr/bin/env python3
"""iOS 27's translated parts, made by Bazel actions as the user (install/translated.bzl).

    translate.py delta --role ROLE --image MEMBER --driver D --tool T [--pins PINS] [--apple-trustcache F]...
                       [--cdhash-pairs F]... --out DIR
    translate.py cryptex --image MEMBER --delta DIR --role ROLE --apfs-tool T --out FILE --sha256 FILE

`delta` mounts the member's volume (the OS image's System volume by the UUID its pins name) read-only and runs the
pretranslate driver over it into DIR. `cryptex` makes the member's translated, sealed cryptex from that delta
(cryptex.py) and its sha256. Both attach disk images, so their actions run locally, unsandboxed. What they log goes
to a file in TMPDIR; a failure prints its end.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(os.path.abspath(__file__)).parents[2]))
from tools.common import hashing
from tools.install import cryptex, delta, disks, host, progress

TAIL_LINES = 40


class FileLog(host.Log):
    """Everything to the file only: Bazel shows an action's output, and a successful one has nothing to say."""

    def __call__(self, *parts: object) -> None:
        self.quiet(*parts)


def _volume(attachment: disks.Attachment, volume_uuid: str | None) -> str:
    if volume_uuid is None:
        if len(attachment.volumes) != 1:
            raise host.HostError(f'expected one APFS volume, got {attachment.volumes}')
        return attachment.volumes[0]
    for volume in attachment.volumes:
        if (host.disk_info(volume) or {}).get('VolumeUUID', '').upper() == volume_uuid.upper():
            return volume
    raise host.HostError(f'no volume {volume_uuid} among {attachment.volumes}')


def make_delta(args: argparse.Namespace, log: host.Log) -> None:
    volume_uuid = None
    if args.role == 'os':
        volume_uuid = json.loads(Path(args.pins).read_text())['volumes']['os_volume_uuid']
    with (
        disks.attached(Path(args.image), log, writable=False) as image,
        disks.mounted(_volume(image, volume_uuid), log, owners=False, read_only=True) as root,
    ):
        if not any(root.iterdir()):
            raise host.HostError(f'{args.role}: the volume mounted at {root} is empty')
        command = [args.driver, '--role', args.role, '--tool', args.tool, '--volume', str(root), '--out', args.out]
        for trust in args.apple_trustcache:
            command += ['--apple-trustcache', trust]
        for pairs in args.cdhash_pairs:
            command += ['--cdhash-pairs', pairs]
        log.quiet('$', ' '.join(command))
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        log.quiet(result.stdout + result.stderr)
        if result.returncode:
            raise host.HostError(f'translating {args.role} failed ({result.returncode})')
    manifest = Path(args.out) / f'{args.role}.manifest.json'
    log(f'{args.role}: {len(delta.load(manifest))} files translated')


def make_cryptex(args: argparse.Namespace, log: host.Log) -> None:
    entries = delta.load(Path(args.delta) / f'{args.role}.manifest.json')
    out = Path(args.out)
    disks.sweep(out.parent, log)
    volume_uuid = uuid.UUID(json.loads(Path(args.pins).read_text())['volumes'][args.role + '_uuid'])
    cryptex.rebuild(cryptex.Job(args.image, entries, out, args.apfs_tool, args.role, volume_uuid), log)
    Path(args.sha256).write_text(hashing.sha256_file(out) + '\n')


def arguments(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    d = commands.add_parser('delta')
    d.add_argument('--role', required=True, choices=('os', 'app_cryptex', 'system_cryptex'))
    d.add_argument('--image', required=True)
    d.add_argument('--pins', help="the IPSW's pins (the os role's System volume UUID)")
    d.add_argument('--driver', required=True)
    d.add_argument('--tool', required=True)
    d.add_argument('--apple-trustcache', action='append', default=[])
    d.add_argument('--cdhash-pairs', action='append', default=[])
    d.add_argument('--out', required=True)
    c = commands.add_parser('cryptex')
    c.add_argument('--image', required=True)
    c.add_argument('--delta', required=True)
    c.add_argument('--role', required=True, choices=('app_cryptex', 'system_cryptex'))
    c.add_argument('--pins', required=True, help="the IPSW's pins (the cryptex's volume UUID)")
    c.add_argument('--apfs-tool', required=True)
    c.add_argument('--out', required=True)
    c.add_argument('--sha256', required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = arguments(argv)
    with tempfile.NamedTemporaryFile('w', prefix='switch-translate-', suffix='.log', delete=False) as f:
        log_path = Path(f.name)
    log = FileLog(log_path)
    progress.SCREEN = progress.Screen(out=log.file, tty=False)
    try:
        with disks.interrupts():
            (make_delta if args.command == 'delta' else make_cryptex)(args, log)
    except (host.HostError, OSError) as error:
        log('FAILED:', error)
        sys.stderr.write(''.join(log_path.read_text().splitlines(keepends=True)[-TAIL_LINES:]))
        return 1
    finally:
        log.file.close()
    log_path.unlink()
    return 0


if __name__ == '__main__':
    sys.exit(main())
