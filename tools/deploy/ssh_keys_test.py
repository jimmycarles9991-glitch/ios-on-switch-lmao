"""Unit tests for external, idempotent SSH authorized_keys deploy staging."""

import base64
import json
import stat
import struct
import tempfile
import unittest
from pathlib import Path

from tools.deploy import ssh_keys


def make_key(key_type: str, body: bytes, comment: str = '') -> str:
    payload = struct.pack('>I', len(key_type)) + key_type.encode('ascii') + body
    blob = base64.b64encode(payload).decode('ascii')
    return f'{key_type} {blob} {comment}'.strip()


KEY_ED_1 = make_key('ssh-ed25519', b'\x01' * 32, 'alice@mac')
KEY_ED_2 = make_key('ssh-ed25519', b'\x02' * 32, 'bob@mac')
KEY_RSA = make_key('ssh-rsa', b'\x03' * 64, 'rsa@mac')
KEY_ECDSA = make_key('ecdsa-sha2-nistp256', b'\x04' * 48, 'ecdsa@mac')
KEY_SK = make_key('sk-ssh-ed25519@openssh.com', b'\x05' * 48, 'yubikey')


class SshKeysTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.home = self.root / 'home'
        (self.home / '.ssh').mkdir(parents=True)
        self.data = self.root / 'data'
        self.data.mkdir()
        self.log: list[str] = []

    def dev_package(self, name: str = 'dev', *, via_manifest: bool = False) -> Path:
        pkg = self.root / name
        if via_manifest:
            manifest = pkg / 'bootstrap/manifest.json'
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps({'managed_packages': {'switch-ssh': {'version': '1+switch1'}}}))
        else:
            deb = pkg / ssh_keys.OVERLAY_DEB
            deb.parent.mkdir(parents=True)
            deb.write_bytes(b'deb')
        return pkg

    def release_package(self, name: str = 'rel') -> Path:
        pkg = self.root / name
        manifest = pkg / 'bootstrap/manifest.json'
        manifest.parent.mkdir(parents=True)
        manifest.write_text(json.dumps({'managed_packages': {'ellekit': {'version': '1'}}}))
        return pkg

    def test_parse_validates_deduplicates_sorts_and_skips_private_keys(self) -> None:
        raw = '\n'.join(
            [
                '# leading comment',
                '',
                '-----BEGIN OPENSSH PRIVATE KEY-----',
                'b3BlbnNzaC1rZXktdjEAAAAABG5vbmU=',
                '-----END OPENSSH PRIVATE KEY-----',
                KEY_RSA,
                KEY_ED_2,
                KEY_ED_1,
                KEY_ED_1.replace('alice@mac', 'zzz@duplicate'),
                KEY_ECDSA,
                KEY_SK,
                'ssh-ed25519 not-valid-base64!!!',
                'ssh-ed25519 ' + base64.b64encode(b'wrong-wire-format').decode('ascii'),
                'command="/bin/sh" ' + KEY_ED_1,
                'unknown-type AAAA',
            ]
        )
        parsed = ssh_keys.parse_public_keys(raw)
        self.assertEqual(parsed, sorted([KEY_ECDSA, KEY_SK, KEY_ED_1, KEY_ED_2, KEY_RSA]))

    def test_resolution_priority_cli_then_env_then_default_home(self) -> None:
        (self.home / '.ssh/id_ed25519.pub').write_text(KEY_ED_1 + '\n')
        (self.home / '.ssh/id_ed25519').write_text('-----BEGIN OPENSSH PRIVATE KEY-----\nx\n')
        env_file = self.root / 'env.pub'
        env_file.write_text(KEY_ED_2 + '\n')
        cli_file = self.root / 'cli.pub'
        cli_file.write_text(KEY_RSA + '\n')
        pkg = self.dev_package()

        default_plan = ssh_keys.load(pkg, self.log.append, ssh_keys.Options(env={}, home=self.home))
        self.assertEqual(default_plan.keys, (KEY_ED_1,))

        env_plan = ssh_keys.load(
            pkg, self.log.append, ssh_keys.Options(env={ssh_keys.ENV_VAR: str(env_file)}, home=self.home)
        )
        self.assertEqual(env_plan.keys, (KEY_ED_2,))

        cli_plan = ssh_keys.load(
            pkg,
            self.log.append,
            ssh_keys.Options(path=cli_file, env={ssh_keys.ENV_VAR: str(env_file)}, home=self.home),
        )
        self.assertEqual(cli_plan.keys, (KEY_RSA,))

        with self.assertRaises(ValueError):
            ssh_keys.load(pkg, self.log.append, ssh_keys.Options(path=self.root / 'missing.pub', env={}))

    def test_idempotent_staging_and_mode_and_release_cleanup(self) -> None:
        (self.home / '.ssh/a.pub').write_text(KEY_ED_2 + '\n')
        (self.home / '.ssh/b.pub').write_text(KEY_ED_1 + '\n')
        dev = self.dev_package(via_manifest=True)
        plan = ssh_keys.load(dev, self.log.append, ssh_keys.Options(env={}, home=self.home))

        self.assertTrue(ssh_keys.stage(self.data, plan, self.log.append))
        staged = self.data / ssh_keys.STAGED
        self.assertEqual(staged.read_text(), f'{KEY_ED_1}\n{KEY_ED_2}\n')
        self.assertEqual(stat.S_IMODE(staged.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(staged.parent.stat().st_mode), 0o700)
        inode = staged.stat().st_ino

        self.assertFalse(ssh_keys.stage(self.data, plan, self.log.append))
        self.assertEqual(staged.stat().st_ino, inode)

        rel_plan = ssh_keys.load(self.release_package(), self.log.append, ssh_keys.Options(env={}, home=self.home))
        self.assertFalse(rel_plan.development)
        self.assertTrue(ssh_keys.stage(self.data, rel_plan, self.log.append))
        self.assertFalse(staged.exists())
        self.assertFalse(staged.parent.exists())
        self.assertFalse(ssh_keys.stage(self.data, rel_plan, self.log.append))

    def test_disabled_or_empty_keys_removes_stale_file(self) -> None:
        dev = self.dev_package()
        (self.home / '.ssh/id.pub').write_text(KEY_ED_1 + '\n')
        plan = ssh_keys.load(dev, self.log.append, ssh_keys.Options(env={}, home=self.home))
        self.assertTrue(ssh_keys.stage(self.data, plan, self.log.append))

        disabled = ssh_keys.load(dev, self.log.append, ssh_keys.Options(disabled=True, env={}, home=self.home))
        self.assertTrue(ssh_keys.stage(self.data, disabled, self.log.append))
        self.assertFalse((self.data / ssh_keys.STAGED).exists())

        self.assertTrue(ssh_keys.stage(self.data, plan, self.log.append))
        empty_home = self.root / 'empty-home'
        empty_home.mkdir()
        empty = ssh_keys.load(dev, self.log.append, ssh_keys.Options(env={}, home=empty_home))
        self.assertEqual(empty.keys, ())
        self.assertTrue(ssh_keys.stage(self.data, empty, self.log.append))
        self.assertFalse((self.data / ssh_keys.STAGED).exists())


if __name__ == '__main__':
    unittest.main()
