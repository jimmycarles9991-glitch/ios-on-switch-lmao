"""Verify the built debug bootstrap manifest and switch-ssh deb carry the SSH set with zero embedded keys."""

import json
import os
import unittest
from pathlib import Path

from bootstrap import development, signing
from tools.common import macho


def runfile(name: str) -> Path:
    (path,) = Path(os.environ['TEST_SRCDIR'], os.environ['TEST_WORKSPACE']).glob(name)
    return path


class Manifest(unittest.TestCase):
    def test_the_debug_manifest_and_deb_carry_the_set_without_any_keys(self) -> None:
        manifest = json.loads(runfile('bootstrap/payload.manifest.json').read_text())
        managed = manifest['managed_packages']
        self.assertLessEqual({*development.UPSTREAM_FILES, 'switch-ssh'}, set(managed))
        self.assertEqual(managed['openssh-server']['version'], development.UPSTREAM + development.SUFFIX)
        self.assertEqual(managed['switch-ssh']['version'], development.SWITCH_SSH_VERSION)
        self.assertEqual(manifest['holds'][4:], sorted([*development.UPSTREAM_FILES, 'switch-ssh']))
        self.assertEqual(manifest['ssh']['user'], 'root')
        self.assertEqual(manifest['ssh']['authorized_keys'], development.AUTHORIZED_STAGED)
        self.assertNotIn('client_key', manifest['ssh'])

        deb = runfile('bootstrap/development.switch-ssh.deb')
        raw = deb.read_bytes()
        self.assertNotIn(b'PRIVATE KEY', raw)
        self.assertNotIn(b'ssh-ed25519 AAAA', raw)
        _, files = development.read_package(deb, runfile('bootstrap/libzstd.dylib'))
        self.assertNotIn(development.AUTHORIZED.removeprefix('/'), files)
        self.assertNotIn(development.HOST_KEY.removeprefix('/'), files)
        self.assertIn(development.WRAPPER.removeprefix('/'), files)
        threads = files[development.THREADS.removeprefix('/')][1]
        signing.check_signature(threads, macho.macho(threads), 'com.brunostjohn.switch.threads')


if __name__ == '__main__':
    unittest.main()
