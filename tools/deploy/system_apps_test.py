"""The datamigrator sentinel the deploy writes when the overlay's /Applications changes."""

import tempfile
import unittest
from pathlib import Path

from tools.deploy import system_apps
from tools.install import card as cards


class Log:
    def __init__(self) -> None:
        self.lines = []

    def __call__(self, *parts: object) -> None:
        self.lines.append(' '.join(map(str, parts)))


def package(root: Path, executable: bytes | None) -> Path:
    root = Path(root)
    tool = root / 'overlay/usr/sbin/BlueTool'
    tool.parent.mkdir(parents=True, exist_ok=True)
    tool.write_bytes(b'tool')
    if executable is not None:
        app = root / 'overlay/Applications/SwitchHWTest.app/SwitchHWTest'
        app.parent.mkdir(parents=True, exist_ok=True)
        app.write_bytes(executable)
    return root


class SystemAppsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.data = self.tmp / 'data'
        self.data.mkdir()
        self.sentinel = self.data / system_apps.SENTINEL

    def deploy(self, executable: bytes | None) -> bool:
        package_root = package(self.tmp / f'package-{executable!r}', executable)
        before = system_apps.digest(self.data)
        cards.sync_overlay(self.data, package_root, Log())
        return system_apps.request_migration(self.data, before, Log())

    def test_a_new_app_asks_for_the_migration(self):
        self.assertTrue(self.deploy(b'v1'))
        self.assertTrue(self.sentinel.is_file())

    def test_an_unchanged_app_does_not(self):
        self.deploy(b'v1')
        self.sentinel.unlink()
        self.assertFalse(self.deploy(b'v1'))
        self.assertFalse(self.sentinel.exists())

    def test_a_rebuilt_app_does(self):
        self.deploy(b'v1')
        self.sentinel.unlink()
        self.assertTrue(self.deploy(b'v2'))

    def test_a_removed_app_does(self):
        self.deploy(b'v1')
        self.sentinel.unlink()
        self.assertTrue(self.deploy(None))
        self.assertTrue(self.sentinel.is_file())

    def test_no_apps_ever_leaves_migration_alone(self):
        self.assertFalse(self.deploy(None))
        self.assertFalse(self.sentinel.exists())


if __name__ == '__main__':
    unittest.main()
