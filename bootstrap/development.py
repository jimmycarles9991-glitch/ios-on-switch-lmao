"""Development-image SSH server over USB and process tools (never included in release images).

Packages Procursus OpenSSH 9.7p1 and Apple's top 125 (re-signed for the rootless bootstrap and versioned +switch1)
together with switch-ssh: a launchd inetd job on loopback port 22, a locked-down sshd_config, the switch-sshd wrapper
that generates the device's host key on Data once and syncs deploy-staged authorized_keys, and switch-threads for
per-thread CPU and per-VM-tag memory snapshots. Zero SSH key bytes enter any package or manifest.
"""

import argparse
import ctypes
import io
import json
import plistlib
import tarfile
import tempfile
from pathlib import Path

from bootstrap import packages, signing

SUFFIX = '+switch1'
UPSTREAM = '9.7p1-1'
SWITCH_SSH_VERSION = '1+switch1'
LABEL = 'com.brunostjohn.switch.sshd'
DIRECTORY = 'var/jb/etc/ssh/switch'
CONFIG = '/' + DIRECTORY + '/sshd_config'
HOST_KEY = '/' + DIRECTORY + '/ssh_host_ed25519_key'
AUTHORIZED = '/' + DIRECTORY + '/authorized_keys'
AUTHORIZED_STAGED = '/private/var/Switch/ssh/authorized_keys'
WRAPPER = '/var/jb/libexec/switch-sshd'
THREADS = '/var/jb/usr/bin/switch-threads'
UPSTREAM_FILES = {
    'openssh-client': 'openssh-client.deb',
    'openssh-sftp-server': 'openssh-sftp-server.deb',
    'openssh-server': 'openssh-server.deb',
    'top': 'top.deb',
}
SERVER_DROPPED = ('var/jb/Library/LaunchDaemons/com.openssh.sshd.plist', 'var/jb/usr/libexec/sshd-keygen-wrapper')
SSHD_CONFIG = f"""# switch-ssh (bootstrap/development.py): root, public-key only, loopback via usbmuxd only.
HostKey {HOST_KEY}
AuthorizedKeysFile {AUTHORIZED}
StrictModes yes
AuthenticationMethods publickey
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
PermitRootLogin prohibit-password
AllowUsers root
AllowTcpForwarding no
AllowStreamLocalForwarding no
AllowAgentForwarding no
X11Forwarding no
PermitTunnel no
GatewayPorts no
PermitUserEnvironment no
Subsystem sftp /var/jb/usr/libexec/sftp-server
"""


def job() -> bytes:
    """Return sshd's launchd inetd plist listening on loopback port 22 only."""
    return plistlib.dumps(
        {
            'Label': LABEL,
            'ProgramArguments': [WRAPPER],
            'Sockets': {'Listeners': {'SockNodeName': 'localhost', 'SockServiceName': '22'}},
            'inetdCompatibility': {'Wait': False},
            'SessionCreate': True,
            'StandardErrorPath': '/dev/null',
        },
        sort_keys=True,
    )


class _ZstdBuffer(ctypes.Structure):
    _fields_ = [('data', ctypes.c_void_p), ('size', ctypes.c_size_t), ('pos', ctypes.c_size_t)]


def decode_zstd_stream(data: bytes, library: Path, limit: int = packages.MAX_FILE) -> bytes:
    """Decompress a zstd frame without recorded content size (Procursus debs) via libzstd's streaming API."""
    codec = ctypes.CDLL(str(library))
    codec.ZSTD_createDCtx.restype = ctypes.c_void_p
    codec.ZSTD_freeDCtx.argtypes = [ctypes.c_void_p]
    codec.ZSTD_decompressStream.argtypes = [ctypes.c_void_p, ctypes.POINTER(_ZstdBuffer), ctypes.POINTER(_ZstdBuffer)]
    codec.ZSTD_decompressStream.restype = ctypes.c_size_t
    codec.ZSTD_isError.argtypes = [ctypes.c_size_t]
    codec.ZSTD_isError.restype = ctypes.c_uint
    context = codec.ZSTD_createDCtx()
    if not context:
        raise MemoryError('zstd context')
    source = ctypes.create_string_buffer(data, len(data))
    given = _ZstdBuffer(ctypes.cast(source, ctypes.c_void_p), len(data), 0)
    chunk = ctypes.create_string_buffer(1 << 20)
    out = bytearray()
    try:
        while True:
            taken = _ZstdBuffer(ctypes.cast(chunk, ctypes.c_void_p), len(chunk), 0)
            left = codec.ZSTD_decompressStream(context, ctypes.byref(taken), ctypes.byref(given))
            if codec.ZSTD_isError(left):
                raise ValueError('zstd frame failed decompression')
            out += chunk.raw[: taken.pos]
            if len(out) > limit:
                raise ValueError('oversized zstd frame')
            if left == 0 and given.pos == given.size:
                return bytes(out)
            if taken.pos == 0 and given.pos == given.size:
                raise ValueError('truncated zstd frame')
    finally:
        codec.ZSTD_freeDCtx(context)


def read_package(path: Path, zstd_lib: Path) -> tuple[dict, dict]:
    """Return a deb's control files and data entries (keeping the var/jb prefix)."""
    result = []
    members = packages.read_deb(path)
    for kind in ('control', 'data'):
        (name,) = (member for member in members if member.startswith(kind + '.tar'))
        data = members[name]
        if name.endswith('.zst'):
            data = decode_zstd_stream(data, zstd_lib)
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            entries = packages.read_tar(archive, strip_prefix=False)
        result.append({key.removeprefix('./'): value for key, value in entries.items()})
    control, files = result
    control.pop('md5sums', None)
    return control, files


def set_fields(control: dict, fields: dict) -> None:
    lines = control['control'][1].decode().strip().splitlines()
    for key, value in fields.items():
        matches = [i for i, line in enumerate(lines) if line.startswith(key + ':')]
        if len(matches) != 1:
            raise ValueError(f'expected one {key} field')
        lines[matches[0]] = f'{key}: {value}'
    packages.add_file(control, 'control', ('\n'.join(lines) + '\n').encode())


def field(control: dict, key: str) -> str:
    values = [
        line[len(key) + 2 :] for line in control['control'][1].decode().splitlines() if line.startswith(key + ': ')
    ]
    if len(values) != 1:
        raise ValueError(f'expected one {key} field')
    return values[0]


def adapt(name: str, control: dict, files: dict) -> None:
    """Version +switch1, pin OpenSSH deps to the adapted client, and strip Procursus' stock sshd job."""
    if field(control, 'Package') != name or field(control, 'Architecture') != 'iphoneos-arm64':
        raise ValueError(f'{name}: not the pinned rootless package')
    fields = {'Version': field(control, 'Version') + SUFFIX}
    if name.startswith('openssh-'):
        if field(control, 'Version') != UPSTREAM:
            raise ValueError(f'{name}: not OpenSSH {UPSTREAM}')
        depends = field(control, 'Depends')
        if name != 'openssh-client':
            pinned = f'openssh-client (= {UPSTREAM})'
            if pinned not in depends:
                raise ValueError(f'{name}: no exact openssh-client dependency')
            fields['Depends'] = depends.replace(pinned, f'openssh-client (= {UPSTREAM}{SUFFIX})')
    set_fields(control, fields)
    if name == 'openssh-server':
        for path in SERVER_DROPPED:
            if path not in files:
                raise ValueError(f'openssh-server: {path} is not where it was pinned')
            del files[path]
        for script in ('extrainst_', 'prerm'):
            control.pop(script, None)


def switch_ssh(wrapper: bytes, threads: bytes) -> tuple[dict, dict, str]:
    """Return hermetic switch-ssh control, files and version with zero embedded SSH keys."""
    control = {}
    packages.add_file(
        control,
        'control',
        (
            'Package: switch-ssh\n'
            f'Version: {SWITCH_SSH_VERSION}\n'
            'Architecture: iphoneos-arm64\n'
            'Maintainer: Switch\n'
            f'Depends: openssh-server (= {UPSTREAM}{SUFFIX}), openssh-sftp-server (= {UPSTREAM}{SUFFIX})\n'
            'Description: Switch development images: loopback-only SSH over USB and per-thread CPU and memory tool\n'
        ).encode(),
    )
    files = {}
    config_dir = tarfile.TarInfo(DIRECTORY)
    config_dir.type, config_dir.mode = tarfile.DIRTYPE, 0o700
    files[DIRECTORY] = config_dir, b''
    packages.add_file(files, f'var/jb/Library/LaunchDaemons/{LABEL}.plist', job())
    packages.add_file(files, CONFIG.removeprefix('/'), SSHD_CONFIG.encode())
    packages.add_file(files, WRAPPER.removeprefix('/'), wrapper, 0o755)
    packages.add_file(files, THREADS.removeprefix('/'), threads, 0o755)
    packages.ensure_directories(files, boundary=packages.PREFIX)
    return control, files, SWITCH_SSH_VERSION


def build(args: argparse.Namespace) -> dict:
    """Write the development debs and record.json into args.out."""
    args.out.mkdir(parents=True, exist_ok=True)
    record = {
        'packages': {},
        'ssh': {
            'authorized_keys': AUTHORIZED_STAGED,
            'host_key': HOST_KEY,
            'listen': 'loopback (127.0.0.1 and ::1) port 22, through launchd',
            'user': 'root',
        },
    }
    with tempfile.TemporaryDirectory(prefix='switch-development-sign-', dir=args.out.parent) as temporary:
        work = Path(temporary)
        for name, filename in UPSTREAM_FILES.items():
            control, files = read_package(getattr(args, name.replace('-', '_')), args.zstd_lib)
            adapt(name, control, files)
            signing.sign(files, work)
            packages.write_deb(args.out / filename, control, files)
            record['packages'][name] = {'file': filename, 'version': field(control, 'Version')}
        control, files, version = switch_ssh(args.wrapper.read_bytes(), args.switch_threads.read_bytes())
        signing.sign(files, work)
        packages.write_deb(args.out / 'switch-ssh.deb', control, files)
        record['packages']['switch-ssh'] = {'file': 'switch-ssh.deb', 'version': version}
    (args.out / 'record.json').write_text(json.dumps(record, sort_keys=True, indent=2) + '\n')
    return record


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in (*UPSTREAM_FILES, 'wrapper', 'switch-threads', 'zstd-lib', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    for name, path in vars(args).items():
        setattr(args, name, path.resolve())
    build(args)


if __name__ == '__main__':
    main()
