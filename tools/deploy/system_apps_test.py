"""The datamigrator sentinel the deploy writes when the overlay's /Applications changes."""

import os
import plistlib
import struct
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


def bundle(app: Path, bundle_id: str, executable: bytes | None = b'arm64e') -> Path:
    app.mkdir(parents=True)
    (app / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': bundle_id, 'CFBundleExecutable': app.stem}))
    if executable is not None:
        (app / app.stem).write_bytes(executable)
        (app / app.stem).chmod(0o755)
    return app


ENTITLEMENTS = {'platform-application': True, 'com.apple.private.security.container-required': True}


def signed(code: bytes, identifier: str = 'com.brunostjohn.switch.hwtest', entitlements: dict = ENTITLEMENTS) -> bytes:
    """Return a thin ARM64 Mach-O: `code`, then a signature with a CodeDirectory and an entitlements blob."""
    ident = identifier.encode() + b'\0'
    directory = struct.pack(
        '>IIIIIIIIIBBBBIII',
        0xFADE0C02,
        52 + len(ident) + 5,
        0x20400,
        2,
        0,
        52,
        0,
        0,
        0,
        32,
        2,
        0,
        12,
        0,
        0,
        52 + len(ident),
    )
    directory += ident + b'TEAM\0'
    xml = plistlib.dumps(entitlements)
    blob = struct.pack('>II', 0xFADE7171, 8 + len(xml)) + xml
    superblob = struct.pack('>III', 0xFADE0CC0, 12 + 16 + len(directory) + len(blob), 2)
    superblob += struct.pack('>II', 0, 28) + struct.pack('>II', 5, 28 + len(directory)) + directory + blob
    offset = 48 + len(code)
    header = struct.pack('<8I', 0xFEEDFACF, 0x100000C, 0, 2, 1, 16, 0, 0) + struct.pack(
        '<4I', 0x1D, 16, offset, len(superblob)
    )
    return header + code + superblob


def package(root: Path, app: dict | None) -> Path:
    """Return a boot package whose overlay has SwitchHWTest.app made from `app` (code, Info.plist, entitlements)."""
    root = Path(root)
    tool = root / 'overlay/usr/sbin/BlueTool'
    tool.parent.mkdir(parents=True, exist_ok=True)
    tool.write_bytes(b'tool')
    if app is not None:
        bundle_root = root / 'overlay/Applications/SwitchHWTest.app'
        bundle_root.mkdir(parents=True, exist_ok=True)
        info = {'CFBundleIdentifier': 'com.brunostjohn.switch.hwtest', 'CFBundleExecutable': 'SwitchHWTest'}
        (bundle_root / 'Info.plist').write_bytes(plistlib.dumps({**info, **app.get('info', {})}))
        (bundle_root / 'SwitchHWTest').write_bytes(
            signed(app['code'], entitlements=app.get('entitlements', ENTITLEMENTS))
        )
        for name, data in app.get('files', {}).items():
            (bundle_root / name).parent.mkdir(parents=True, exist_ok=True)
            (bundle_root / name).write_bytes(data)
    return root


class SystemAppsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.data = self.tmp / 'data'
        self.data.mkdir()
        self.sentinel = self.data / system_apps.SENTINEL

    def deploy(self, app: dict | None) -> bool:
        package_root = package(self.tmp / f'package-{len(list(self.tmp.iterdir()))}', app)
        before = system_apps.digest(self.data)
        cards.sync_overlay(self.data, package_root, Log())
        return system_apps.request_migration(self.data, before, Log())

    def redeploy(self, app: dict | None) -> bool:
        self.sentinel.unlink(missing_ok=True)
        return self.deploy(app)

    def test_a_new_app_asks_for_the_migration(self):
        self.assertTrue(self.deploy({'code': b'v1'}))
        self.assertTrue(self.sentinel.is_file())

    def test_an_unchanged_app_does_not(self):
        self.deploy({'code': b'v1'})
        self.assertFalse(self.redeploy({'code': b'v1'}))
        self.assertFalse(self.sentinel.exists())

    def test_a_rebuilt_executable_or_resource_alone_does_not(self):
        self.deploy({'code': b'v1', 'files': {'default.metallib': b'a', 'media/clip.mp4': b'a'}})
        self.assertFalse(
            self.redeploy({'code': b'v2 longer', 'files': {'default.metallib': b'b', 'media/clip.mp4': b'b'}})
        )
        self.assertFalse(self.sentinel.exists())

    def test_an_info_plist_change_does(self):
        self.deploy({'code': b'v1'})
        self.assertTrue(self.redeploy({'code': b'v1', 'info': {'CFBundleDisplayName': 'HW Test 2'}}))

    def test_a_localized_name_change_does(self):
        self.deploy({'code': b'v1', 'files': {'en.lproj/InfoPlist.strings': b'"CFBundleDisplayName" = "A";'}})
        self.assertTrue(
            self.redeploy({'code': b'v1', 'files': {'en.lproj/InfoPlist.strings': b'"CFBundleDisplayName" = "B";'}})
        )

    def test_an_icon_or_its_asset_catalog_changing_does(self):
        icons = {'CFBundleIcons': {'CFBundlePrimaryIcon': {'CFBundleIconFiles': ['AppIcon60x60']}}}
        self.deploy({'code': b'v1', 'info': icons, 'files': {'AppIcon60x60@2x.png': b'a', 'Assets.car': b'a'}})
        self.assertTrue(
            self.redeploy({'code': b'v1', 'info': icons, 'files': {'AppIcon60x60@2x.png': b'b', 'Assets.car': b'a'}})
        )
        self.assertTrue(
            self.redeploy({'code': b'v1', 'info': icons, 'files': {'AppIcon60x60@2x.png': b'b', 'Assets.car': b'b'}})
        )

    def test_an_entitlements_change_does(self):
        self.deploy({'code': b'v1'})
        self.assertTrue(self.redeploy({'code': b'v1', 'entitlements': {**ENTITLEMENTS, 'com.apple.private.x': True}}))

    def test_an_added_extension_does(self):
        self.deploy({'code': b'v1'})
        extension = {'PlugIns/W.appex/Info.plist': plistlib.dumps({'CFBundleIdentifier': 'w'})}
        self.assertTrue(self.redeploy({'code': b'v1', 'files': extension}))

    def test_a_removed_app_does(self):
        self.deploy({'code': b'v1'})
        self.assertTrue(self.redeploy(None))
        self.assertTrue(self.sentinel.is_file())

    def test_no_apps_ever_leaves_migration_alone(self):
        self.assertFalse(self.deploy(None))
        self.assertFalse(self.sentinel.exists())

    def test_a_request_clears_the_kernels_one_try_marker(self):
        marker = self.data / system_apps.MARKER
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.touch()
        self.assertTrue(self.deploy({'code': b'v1'}))
        self.assertFalse(marker.exists())

    def test_force_asks_with_unchanged_apps(self):
        self.deploy({'code': b'v1'})
        self.sentinel.unlink()
        before = system_apps.digest(self.data)
        self.assertTrue(system_apps.request_migration(self.data, before, Log(), force=True))
        self.assertTrue(self.sentinel.is_file())


class LaunchServicesRebuildTest(unittest.TestCase):
    def setUp(self):
        self.data = Path(tempfile.mkdtemp())
        caches = self.data / 'mobile/Containers/Data/InternalDaemon/097FB1B3/Library/Caches'
        caches.mkdir(parents=True)
        self.store = caches / 'com.apple.LaunchServices-35651614-v2.csstore'
        self.store.write_bytes(b'store')
        self.other = caches / 'com.apple.LaunchServices.dirty'
        self.other.write_bytes(b'')

    def test_ios_27_rebuilds_and_17_does_not(self):
        self.assertTrue(system_apps.rebuilds_launchservices('27.2'))
        self.assertFalse(system_apps.rebuilds_launchservices('17.7.11'))
        self.assertFalse(system_apps.rebuilds_launchservices(None))

    def test_a_requested_rebuild_removes_only_the_store(self):
        system_apps.request_migration(self.data, None, Log(), force=True, rebuild=True)
        self.assertFalse(self.store.exists())
        self.assertTrue(self.other.exists())
        self.assertTrue((self.data / system_apps.SENTINEL).is_file())

    def test_a_migration_without_rebuild_keeps_the_store(self):
        system_apps.request_migration(self.data, None, Log(), force=True)
        self.assertTrue(self.store.exists())

    def test_no_migration_no_rebuild(self):
        self.assertFalse(system_apps.request_migration(self.data, system_apps.digest(self.data), Log(), rebuild=True))
        self.assertTrue(self.store.exists())


class RestageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.system, self.data = self.tmp / 'system', self.tmp / 'data'
        staged = self.system / 'private/var' / system_apps.STAGED
        self.facetime = bundle(staged / 'FaceTime.app', 'com.apple.facetime')
        (self.facetime / 'en.lproj').mkdir()
        (self.facetime / 'en.lproj/InfoPlist.strings').write_bytes(b'strings')
        (self.facetime / 'Current').symlink_to('en.lproj')
        bundle(staged / 'Fitness.app', 'com.apple.Fitness')
        bundle(staged / 'Files.app', 'com.apple.DocumentsApp')
        (self.data / system_apps.STAGED).mkdir(parents=True)
        self.states({'com.apple.facetime': 1, 'com.apple.Fitness': 2, 'com.apple.DocumentsApp': 1})

    def states(self, states: dict) -> None:
        path = self.data / system_apps.APP_STATES
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(plistlib.dumps(states))

    def install(self, name: str, executable: bytes | None) -> None:
        bundle(self.data / system_apps.INSTALLED / name.upper() / name, 'id', executable)

    def test_consumed_apps_the_migration_still_wants_come_back_whole(self):
        log = Log()
        self.assertTrue(system_apps.restage(self.system, self.data, log))
        copy = self.data / system_apps.STAGED / 'FaceTime.app'
        self.assertEqual((copy / 'FaceTime').read_bytes(), b'arm64e')
        self.assertEqual(os.stat(copy / 'FaceTime').st_mode & 0o777, 0o755)
        self.assertEqual((copy / 'en.lproj/InfoPlist.strings').read_bytes(), b'strings')
        self.assertEqual(os.readlink(copy / 'Current'), 'en.lproj')
        self.assertTrue((self.data / system_apps.STAGED / 'Files.app').is_dir())
        self.assertIn('FaceTime.app', log.lines[0])

    def test_an_app_marked_uninstalled_stays_out(self):
        system_apps.restage(self.system, self.data, Log())
        self.assertFalse((self.data / system_apps.STAGED / 'Fitness.app').exists())

    def test_restaging_twice_copies_once_and_asks_again(self):
        self.assertTrue(system_apps.restage(self.system, self.data, Log()))
        stamp = (self.data / system_apps.STAGED / 'FaceTime.app/FaceTime').stat().st_ino
        log = Log()
        self.assertTrue(system_apps.restage(self.system, self.data, log))
        self.assertEqual((self.data / system_apps.STAGED / 'FaceTime.app/FaceTime').stat().st_ino, stamp)
        self.assertIn('0 restaged', log.lines[0])

    def test_installed_apps_need_nothing(self):
        self.install('FaceTime.app', b'arm64e')
        self.install('Files.app', b'arm64e')
        self.assertFalse(system_apps.restage(self.system, self.data, Log()))

    def test_an_installed_app_is_left_alone(self):
        self.install('FaceTime.app', b'arm64e')
        self.install('Files.app', b'arm64e')
        self.assertEqual(system_apps.missing(self.system, self.data), [])

    def test_a_placeholder_without_its_executable_is_not_installed(self):
        self.install('FaceTime.app', None)
        self.install('Files.app', b'arm64e')
        self.assertEqual(system_apps.missing(self.system, self.data), ['FaceTime.app'])

    def test_without_mobile_installations_states_nothing_is_restaged(self):
        (self.data / system_apps.APP_STATES).unlink()
        self.assertFalse(system_apps.restage(self.system, self.data, Log()))

    def test_a_copy_cut_short_is_replaced(self):
        partial = self.data / system_apps.STAGED / '.FaceTime.app.partial'
        partial.mkdir()
        (partial / 'junk').write_bytes(b'')
        system_apps.restage(self.system, self.data, Log())
        self.assertFalse(partial.exists())
        self.assertFalse((self.data / system_apps.STAGED / 'FaceTime.app/junk').exists())

    def test_restaging_asks_for_the_migration(self):
        before = system_apps.digest(self.data)
        restaged = system_apps.restage(self.system, self.data, Log())
        self.assertTrue(system_apps.request_migration(self.data, before, Log(), force=restaged))
        self.assertTrue((self.data / system_apps.SENTINEL).is_file())


if __name__ == '__main__':
    unittest.main()
