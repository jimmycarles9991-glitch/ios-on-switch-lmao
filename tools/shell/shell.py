"""Root shell and process/thread snapshots on a development image over USB.

    bazel run //tools/shell -- [-i KEY] [--udid UDID] shell
    bazel run //tools/shell -- [-i KEY] [--udid UDID] top OUT
    bazel run //tools/shell -- [-i KEY] [--udid UDID] ps OUT
    bazel run //tools/shell -- [-i KEY] [--udid UDID] threads OUT [-- SWITCH_THREADS_ARGS...]
    bazel run //tools/shell -- [-i KEY] [--udid UDID] run OUT -- COMMAND...

Connects through libusbmuxd (`inetcat 22`, zero host network listener) using the user's standard SSH keys or
ssh-agent, pinning each device's host key per UDID under ~/Library/Application Support/switch-ios/known_hosts.
"""

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from tools.common import paths

ALIAS_PREFIX = 'switch-dev-'
TOP = '/var/jb/usr/bin/top'
THREADS = '/var/jb/usr/bin/switch-threads'
# Two samples a second apart: top's first CPU sample has no interval behind it.
TOP_ARGS = [
    TOP,
    '-l',
    '2',
    '-s',
    '1',
    '-o',
    'cpu',
    '-n',
    '400',
    '-stats',
    'pid,ppid,command,cpu,time,threads,mem,state',
]
PS_ARGS = [TOP, '-l', '1', '-o', 'pid', '-n', '2000', '-stats', 'pid,ppid,uid,command,threads,mem,state,time']
THREADS_ARGS = [THREADS]
_UDID = re.compile(r'^[A-Za-z0-9._-]+$')


@dataclass(frozen=True)
class SshOptions:
    identity: Path | None = None
    remote: tuple[str, ...] = ()
    batch: bool = False


def os_cwd() -> str:
    """Return the directory `bazel run` was invoked in so relative paths resolve where typed."""
    return os.environ.get('BUILD_WORKING_DIRECTORY', os.getcwd())


def resolve_user_path(path: Path) -> Path:
    expanded = path.expanduser()
    return expanded if expanded.is_absolute() else Path(os_cwd()) / expanded


def default_known_hosts() -> Path:
    return paths.state_dir() / 'known_hosts'


def discover_udid(explicit: str | None) -> str:
    """Return `explicit` or the single USB-attached iOS device UDID from `idevice_id -l`."""
    if explicit is not None:
        if not _UDID.fullmatch(explicit):
            raise SystemExit(f'invalid device UDID: {explicit!r}')
        return explicit
    idevice_id = shutil.which('idevice_id')
    if not idevice_id:
        raise SystemExit('idevice_id (`brew install libimobiledevice`) is not on PATH; install it or pass --udid UDID')
    result = subprocess.run([idevice_id, '-l'], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        detail = result.stderr.strip() or f'exit {result.returncode}'
        raise SystemExit(f'idevice_id -l failed: {detail}')
    devices = list(dict.fromkeys(line.strip() for line in result.stdout.splitlines() if line.strip()))
    if not devices:
        raise SystemExit('no iOS device found over USB (attach the device or pass --udid UDID)')
    if len(devices) > 1:
        raise SystemExit(f'multiple iOS devices attached ({", ".join(devices)}); pass --udid UDID')
    udid = devices[0]
    if not _UDID.fullmatch(udid):
        raise SystemExit(f'unexpected UDID from idevice_id: {udid!r}')
    return udid


def ssh_command(udid: str, known_hosts: Path, inetcat: str, options: SshOptions) -> list[str]:
    alias = f'{ALIAS_PREFIX}{udid}'
    proxy = f'{shlex.quote(inetcat)} -u {shlex.quote(udid)} 22'
    command = ['ssh']
    if options.identity is not None:
        command += ['-i', str(options.identity), '-o', 'IdentitiesOnly=yes']
    command += [
        '-o',
        f'UserKnownHostsFile="{known_hosts}"',
        '-o',
        'StrictHostKeyChecking=accept-new',
        '-o',
        f'HostKeyAlias={alias}',
        '-o',
        f'ProxyCommand={proxy}',
    ]
    if options.batch:
        command += ['-o', 'BatchMode=yes', '-T']
    return [*command, f'root@{alias}', *options.remote]


def remote(args: argparse.Namespace) -> list[str]:
    if args.action == 'top':
        return list(TOP_ARGS)
    if args.action == 'ps':
        return list(PS_ARGS)
    if args.action == 'threads':
        return [*THREADS_ARGS, *args.command]
    if args.action == 'run':
        if not args.command:
            raise SystemExit('run: give the command after --')
        return list(args.command)
    return []


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('-i', '--identity', type=Path, help='private key file (default: ~/.ssh and ssh-agent)')
    parser.add_argument('--udid', help='target device UDID (default: discover via idevice_id -l)')
    parser.add_argument('--known-hosts', type=Path, help='known_hosts file override')
    parser.add_argument('action', choices=['shell', 'top', 'ps', 'threads', 'run'])
    parser.add_argument('out', nargs='?', type=Path, help='output file (top, ps, threads, run)')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if args.command[:1] == ['--']:
        args.command = args.command[1:]
    if args.action != 'shell' and args.out is None:
        raise SystemExit(f'{args.action}: give the file to write')
    inetcat = shutil.which('inetcat')
    if not inetcat:
        raise SystemExit(
            'inetcat (libusbmuxd, `brew install libimobiledevice`) is not on PATH: it carries ssh over USB'
        )
    udid = discover_udid(args.udid)
    known_hosts = resolve_user_path(args.known_hosts) if args.known_hosts else default_known_hosts()
    known_hosts.parent.mkdir(parents=True, exist_ok=True)
    identity = resolve_user_path(args.identity) if args.identity else None
    options = SshOptions(identity=identity, remote=tuple(remote(args)), batch=args.action != 'shell')
    command = ssh_command(udid, known_hosts, inetcat, options)
    if args.action == 'shell':
        return subprocess.run(command, check=False).returncode
    out = resolve_user_path(args.out)
    with out.open('wb') as stream:
        result = subprocess.run(command, stdout=stream, check=False)
    if result.returncode:
        print(
            f'ssh exited {result.returncode}: is a development image attached and booted with your public key staged?',
            file=sys.stderr,
        )
    else:
        print(f'wrote {out}', file=sys.stderr)
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())
