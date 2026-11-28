import argparse
import json
import os
import plistlib
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

from bootstrap import development, packages

STOCK_JOB = 'var/jb/Library/LaunchDaemons/com.openssh.sshd.plist'


def runfile(name: str) -> Path:
    (path,) = Path(os.environ['TEST_SRCDIR'], os.environ['TEST_WORKSPACE']).glob(name)
    return path


def deb(path: Path, spec: tuple[str, str, str], files: dict, scripts: tuple[str, ...] = ()) -> None:
    name, version, depends = spec
    control = {}
    packages.add_file(
        control,
        'control',
        f'Package: {name}\nVersion: {version}\nArchitecture: iphoneos-arm64\nDepends: {depends}\n'.encode(),
    )
    for script in scripts:
        packages.add_file(
            control, script, b'#!/bin/sh\n/var/jb/bin/launchctl load -w ' + STOCK_JOB.encode() + b'\n', 0o755
        )
    entries = {}
    for file, data in files.items():
        packages.add_file(entries, file, data, 0o755)
    packages.write_deb(path, control, entries)


class Development(unittest.TestCase):
    def build(self, root: Path) -> dict:
        upstream = root / 'upstream'
        upstream.mkdir()
        client = 'libiosexec1 (>= 1.3.1), libssl3, libcrypt2, libmd0'
        pinned = 'libiosexec1 (>= 1.3.1), libssl3, openssh-client (= 9.7p1-1), libcrypt2, libmd0'
        deb(upstream / 'client.deb', ('openssh-client', '9.7p1-1', client), {'var/jb/usr/bin/ssh': b'ssh'})
        deb(
            upstream / 'sftp.deb',
            ('openssh-sftp-server', '9.7p1-1', pinned),
            {'var/jb/usr/libexec/sftp-server': b'sftp-server'},
        )
        deb(
            upstream / 'server.deb',
            ('openssh-server', '9.7p1-1', pinned + ', openssh-sftp-server, libpam2'),
            {
                'var/jb/usr/sbin/sshd': b'sshd',
                'var/jb/etc/ssh/sshd_config': b'PermitRootLogin yes\nUsePAM yes\n',
                STOCK_JOB: b'<plist/>',
                'var/jb/usr/libexec/sshd-keygen-wrapper': b'#!/var/jb/bin/sh\n',
            },
            ('extrainst_', 'prerm'),
        )
        deb(
            upstream / 'top.deb',
            ('top', '125', 'libiosexec1 (>= 1.2.2), libncursesw6'),
            {'var/jb/usr/bin/top': b'top'},
        )
        threads = root / 'switch-threads'
        threads.write_bytes(b'switch-threads-stub')
        args = argparse.Namespace(
            openssh_client=upstream / 'client.deb',
            openssh_sftp_server=upstream / 'sftp.deb',
            openssh_server=upstream / 'server.deb',
            top=upstream / 'top.deb',
            wrapper=runfile('bootstrap/switch-sshd.sh'),
            switch_threads=threads,
            zstd_lib=root / 'unused',
            out=root / 'out',
        )
        return development.build(args)

    def test_switch_ssh_is_hermetic_with_zero_embedded_keys(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            record = self.build(root)
            control, files = development.read_package(root / 'out/switch-ssh.deb', root / 'unused')
            job = plistlib.loads(files['var/jb/Library/LaunchDaemons/com.brunostjohn.switch.sshd.plist'][1])
            self.assertEqual(job['Sockets'], {'Listeners': {'SockNodeName': 'localhost', 'SockServiceName': '22'}})
            self.assertEqual(job['ProgramArguments'], [development.WRAPPER])
            config = files[development.CONFIG.removeprefix('/')][1].decode().splitlines()
            for line in (
                f'HostKey {development.HOST_KEY}',
                f'AuthorizedKeysFile {development.AUTHORIZED}',
                'StrictModes yes',
                'AuthenticationMethods publickey',
                'PubkeyAuthentication yes',
                'PasswordAuthentication no',
                'KbdInteractiveAuthentication no',
                'UsePAM no',
                'PermitRootLogin prohibit-password',
                'AllowUsers root',
                'AllowTcpForwarding no',
                'AllowStreamLocalForwarding no',
                'AllowAgentForwarding no',
                'X11Forwarding no',
                'PermitTunnel no',
                'Subsystem sftp /var/jb/usr/libexec/sftp-server',
            ):
                self.assertIn(line, config)
            self.assertNotIn('PermitRootLogin yes', config)
            self.assertNotIn(development.AUTHORIZED.removeprefix('/'), files)
            self.assertNotIn(development.HOST_KEY.removeprefix('/'), files)
            self.assertNotIn(development.HOST_KEY.removeprefix('/') + '.pub', files)
            self.assertEqual(files[development.DIRECTORY][0].mode, 0o700)
            self.assertEqual(files[development.WRAPPER.removeprefix('/')][0].mode, 0o755)
            self.assertEqual(files[development.THREADS.removeprefix('/')][0].mode, 0o755)
            self.assertIn('Depends: openssh-server (= 9.7p1-1+switch1)', control['control'][1].decode())
            self.assertEqual(record['packages']['switch-ssh']['version'], development.SWITCH_SSH_VERSION)
            self.assertEqual(record['ssh']['user'], 'root')
            self.assertEqual(record['ssh']['authorized_keys'], development.AUTHORIZED_STAGED)
            self.assertNotIn('client_key', record['ssh'])

    def test_switch_ssh_is_byte_identical_across_builds(self) -> None:
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            self.build(Path(a))
            self.build(Path(b))
            self.assertEqual(
                (Path(a) / 'out/switch-ssh.deb').read_bytes(), (Path(b) / 'out/switch-ssh.deb').read_bytes()
            )

    def test_procursus_job_and_password_path_leave_the_server_package(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            record = self.build(root)
            control, files = development.read_package(root / 'out/openssh-server.deb', root / 'unused')
            self.assertNotIn(STOCK_JOB, files)
            self.assertNotIn('var/jb/usr/libexec/sshd-keygen-wrapper', files)
            self.assertNotIn('extrainst_', control)
            self.assertNotIn('prerm', control)
            text = control['control'][1].decode()
            self.assertIn('Version: 9.7p1-1+switch1', text)
            self.assertIn('openssh-client (= 9.7p1-1+switch1)', text)
            self.assertEqual(
                {name: item['version'] for name, item in record['packages'].items()},
                {
                    'openssh-client': '9.7p1-1+switch1',
                    'openssh-sftp-server': '9.7p1-1+switch1',
                    'openssh-server': '9.7p1-1+switch1',
                    'switch-ssh': '1+switch1',
                    'top': '125+switch1',
                },
            )
            on_disk = json.loads((root / 'out/record.json').read_text())
            self.assertEqual(on_disk, record)
            self.assertEqual(
                sorted(p.name for p in (root / 'out').iterdir()),
                sorted([*development.UPSTREAM_FILES.values(), 'switch-ssh.deb', 'record.json']),
            )

    def test_wrapper_generates_host_key_once_and_syncs_or_fails_closed_on_keys(self) -> None:
        wrapper = runfile('bootstrap/switch-sshd.sh').read_text()
        subprocess.run(['/bin/sh', '-n', str(runfile('bootstrap/switch-sshd.sh'))], check=True)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            switch_dir = root / 'etc/ssh/switch'
            staged_src = root / 'Switch/ssh/authorized_keys'
            fake_bin = root / 'bin'
            fake_bin.mkdir()
            (fake_bin / 'chown').write_text('#!/bin/sh\nexit 0\n')
            (fake_bin / 'chown').chmod(0o755)
            keygen_log = root / 'keygen.log'
            (fake_bin / 'ssh-keygen').write_text(
                f'#!/bin/sh\n'
                f'out=""\n'
                f'while [ $# -gt 0 ]; do\n'
                f'  if [ "$1" = "-f" ]; then out="$2"; shift 2; else shift; fi\n'
                f'done\n'
                f'echo "$out" >> "{keygen_log}"\n'
                f'printf "PRIVATE-KEY\\n" > "$out"\n'
                f'printf "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIhost test\\n" > "${{out}}.pub"\n'
            )
            (fake_bin / 'ssh-keygen').chmod(0o755)
            sshd_log = root / 'sshd.log'
            (fake_bin / 'sshd').write_text(f'#!/bin/sh\nprintf "%s\\n" "$@" > "{sshd_log}"\n')
            (fake_bin / 'sshd').chmod(0o755)

            rewritten = (
                wrapper.replace('#!/var/jb/bin/sh', '#!/bin/sh')
                .replace(
                    'PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin',
                    f'PATH={fake_bin}:/usr/bin:/bin',
                )
                .replace('DIR=/var/jb/etc/ssh/switch', f'DIR={switch_dir}')
                .replace('AUTH_SRC=/private/var/Switch/ssh/authorized_keys', f'AUTH_SRC={staged_src}')
                .replace('/var/jb/usr/bin/ssh-keygen', str(fake_bin / 'ssh-keygen'))
                .replace('/var/jb/usr/sbin/sshd', str(fake_bin / 'sshd'))
            )
            script = root / 'switch-sshd'
            script.write_text(rewritten)
            script.chmod(0o755)

            staged_src.parent.mkdir(parents=True)
            valid_key = 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA dev@mac\n'
            staged_src.write_text('# comment\n\n' + valid_key)

            first = subprocess.run([str(script)], check=True, capture_output=True)
            self.assertEqual(first.stdout, b'')
            self.assertEqual(stat.S_IMODE(switch_dir.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE((switch_dir / 'ssh_host_ed25519_key').stat().st_mode), 0o600)
            self.assertEqual((switch_dir / 'ssh_host_ed25519_key').read_text(), 'PRIVATE-KEY\n')
            self.assertEqual(stat.S_IMODE((switch_dir / 'authorized_keys').stat().st_mode), 0o600)
            self.assertEqual((switch_dir / 'authorized_keys').read_text(), valid_key)
            self.assertEqual(sshd_log.read_text().splitlines(), ['-i', '-f', str(switch_dir / 'sshd_config')])
            self.assertEqual(len(keygen_log.read_text().splitlines()), 1)

            staged_src.unlink()
            second = subprocess.run([str(script)], check=True, capture_output=True)
            self.assertEqual(second.stdout, b'')
            self.assertEqual(len(keygen_log.read_text().splitlines()), 1)
            self.assertFalse((switch_dir / 'authorized_keys').exists())

            other = root / 'other'
            other.write_text(valid_key)
            staged_src.symlink_to(other)
            subprocess.run([str(script)], check=True, capture_output=True)
            self.assertFalse((switch_dir / 'authorized_keys').exists())

    def test_the_image_carries_the_set_in_debug_packages_only(self) -> None:
        manifest = json.loads((Path(__file__).absolute().parents[1] / 'overlay/manifest.json').read_text())
        entries = {entry['path']: entry for entry in manifest['files']}
        for name in [*development.UPSTREAM_FILES, 'switch-ssh']:
            entry = entries['/usr/share/Switch/bootstrap/packages/' + name + '.deb']
            self.assertEqual(entry['source'], 'build/switch-bootstrap/packages/' + name + '.deb')
            self.assertEqual((entry['union'], entry.get('debug')), ('/usr/share', True))
            self.assertNotIn('trust', entry)

    def test_procursus_streaming_zstd_frames_decode(self) -> None:
        frame = bytes.fromhex('28b52ffd0458990000737769746368207a7374642073747265616d0a57f7194e')
        library = runfile('bootstrap/libzstd.dylib')
        self.assertEqual(development.decode_zstd_stream(frame, library), b'switch zstd stream\n')
        with self.assertRaises(ValueError):
            development.decode_zstd_stream(frame[:-6], library)


if __name__ == '__main__':
    unittest.main()
