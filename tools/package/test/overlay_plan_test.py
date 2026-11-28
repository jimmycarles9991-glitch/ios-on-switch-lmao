"""overlay.py's plan of the real overlay/manifest.json per IPSW kind.

An entry with a kind is staged and unioned only for that kind, and the other kind's sources are not asked of the build.
"""

import importlib.util
import json
import plistlib
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import ModuleType
from unittest import mock

from python.runfiles import runfiles

KINDS = ('21H461', '24B119')


def load_overlay() -> ModuleType:
    path = runfiles.Create().Rlocation('_main/tools/package/overlay.py')
    spec = importlib.util.spec_from_file_location('overlay', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def manifest() -> dict:
    with open(runfiles.Create().Rlocation('_main/overlay/manifest.json')) as handle:
        return json.load(handle)


class KindTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.overlay = load_overlay()
        cls.manifest = manifest()

    def plan(
        self,
        kind: str,
        given: list[str] | None = None,
        *,
        release: bool = True,
        hwtest: bool | None = None,
    ) -> dict:
        return self.overlay.plan(
            self.manifest,
            False,
            kind,
            given,
            self.overlay.BuildOptions(release=release, hwtest=hwtest),
        )

    def kinded(self) -> list[str]:
        return sorted({entry['kind'] for entry in self.manifest['files'] if 'kind' in entry})

    def test_manifest_kinds_are_ipsw_kinds(self):
        self.assertTrue(self.kinded())
        self.assertLessEqual(set(self.kinded()), set(KINDS))

    def released(self) -> list[dict]:
        return [e for e in self.manifest['files'] if not (e.get('debug') or e.get('hwtest'))]

    @staticmethod
    def union(entry: dict) -> str:
        return entry.get('union', entry['path'].rpartition('/')[0])

    def test_each_kind_unions_only_its_own_bundles(self):
        shared = {self.union(e) for e in self.released() if 'kind' not in e}
        for kind in KINDS:
            mine = {self.union(e) for e in self.released() if e.get('kind') == kind}
            others = {self.union(e) for e in self.released() if e.get('kind', kind) != kind} - shared
            dirs = {d for d, _ in self.plan(kind)['dirs']}
            self.assertLessEqual(mine, dirs, kind)
            self.assertFalse(others & dirs, kind)

    def test_avd_sits_in_each_kinds_empty_slot(self):
        slots = {'21H461': 'AVD.videodecoder', '24B119': 'AVD.videodecoder_ps'}
        for kind, slot in slots.items():
            avd = [f['path'] for f in self.plan(kind)['files'] if f['source'] == 'build/switch-video/AVD.videodecoder']
            self.assertEqual(avd, [f'/System/Library/VideoDecoders/{slot}'], kind)

    def test_entries_without_a_kind_are_planned_for_both(self):
        unkinded = {
            e['path']
            for e in self.manifest['files']
            if not ('kind' in e or e.get('debug') or e.get('hwtest') or e.get('portrait'))
        }
        for kind in KINDS:
            files = self.plan(kind)['files']
            kinded = {e['path'] for e in self.manifest['files'] if e.get('kind') == kind}
            self.assertEqual({f['path'] for f in files} - kinded, unkinded, kind)

    def test_managed_packages_are_readable_at_the_workers_paths_in_both_profiles(self):
        base = {'sileo.deb', 'ellekit.deb', 'uikittools.deb', 'launchctl.deb'}
        dev = {'openssh-client.deb', 'openssh-sftp-server.deb', 'openssh-server.deb', 'switch-ssh.deb', 'top.deb'}
        for kind in KINDS:
            for release in (False, True):
                names = base if release else base | dev
                expected = {
                    f'/usr/share/Switch/bootstrap/packages/{name}': f'build/switch-bootstrap/packages/{name}'
                    for name in names
                }
                with self.subTest(kind=kind, release=release):
                    plan = self.plan(kind, release=release)
                    packages = [
                        entry
                        for entry in plan['files']
                        if entry['path'].startswith('/usr/share/Switch/bootstrap/packages/')
                    ]
                    self.assertEqual({entry['path']: entry['source'] for entry in packages}, expected)
                    self.assertTrue(all(entry['mode'] == 0o644 and not entry['trust'] for entry in packages))
                    self.assertIn(['/usr/share', ''], plan['dirs'])

    def test_install_policy_is_trusted_in_both_profiles_and_kinds(self):
        expected = {
            '/usr/libexec/installd': 'build/switch-apps/installd',
            '/usr/lib/libSwitchInstallPolicy.dylib': 'build/switch-apps/libSwitchInstallPolicy.dylib',
        }
        for kind in KINDS:
            for release in (False, True):
                with self.subTest(kind=kind, release=release):
                    plan = self.plan(kind, release=release)
                    entries = {entry['path']: entry for entry in plan['files']}
                    for path, source in expected.items():
                        self.assertEqual(entries[path]['source'], source)
                        self.assertTrue(entries[path]['trust'])
                        self.assertEqual(entries[path]['mode'], 0o755)
                        self.assertIn([entries[path]['directory'], ''], plan['dirs'])

    def test_a_debug_package_carries_its_kinds_test_apps(self):
        apps = {
            kind: sorted(f['path'] for f in self.plan(kind, release=False)['files'] if f['bundle']) for kind in KINDS
        }
        self.assertEqual(
            apps['21H461'],
            ['/Applications/SwitchHWTest.app', '/Applications/SwitchInstaller.app'],
        )
        self.assertEqual(
            apps['24B119'],
            [
                '/Applications/SwitchHWTest.app',
                '/Applications/SwitchHWTest27e.app',
                '/Applications/SwitchInstaller.app',
            ],
        )
        for kind in KINDS:
            self.assertEqual(
                [f['path'] for f in self.plan(kind)['files'] if f['bundle']],
                ['/Applications/SwitchInstaller.app'],
                kind,
            )

    def test_hwtest_can_be_opted_in_on_release_and_opted_out_on_debug(self):
        hwtest_paths = {
            '21H461': {
                '/Applications/SwitchHWTest.app',
                '/usr/bin/switch-texpeer',
            },
            '24B119': {
                '/Applications/SwitchHWTest.app',
                '/Applications/SwitchHWTest27e.app',
                '/usr/bin/switch-texpeer',
                '/private/preboot/Cryptexes/Supplemental/System/Library/LaunchDaemons/com.brunostjohn.switch.texpeer.plist',
            },
        }
        debug_only = {
            '/usr/bin/switch-fetchform',
            '/usr/share/Switch/bootstrap/packages/switch-ssh.deb',
        }
        for kind in KINDS:
            with self.subTest(kind=kind):
                release_default = {f['path'] for f in self.plan(kind, release=True)['files']}
                self.assertFalse(hwtest_paths[kind] & release_default)
                self.assertFalse(debug_only & release_default)

                release_hwtest = {f['path'] for f in self.plan(kind, release=True, hwtest=True)['files']}
                self.assertLessEqual(hwtest_paths[kind], release_hwtest)
                self.assertFalse(debug_only & release_hwtest)

                debug_no_hwtest = {f['path'] for f in self.plan(kind, release=False, hwtest=False)['files']}
                self.assertFalse(hwtest_paths[kind] & debug_no_hwtest)
                self.assertLessEqual(debug_only, debug_no_hwtest)

    def test_installer_is_trusted_in_both_profiles_and_kinds(self):
        for kind in KINDS:
            for release in (False, True):
                with self.subTest(kind=kind, release=release):
                    plan = self.plan(kind, release=release)
                    [entry] = [
                        entry for entry in plan['files'] if entry['source'] == 'build/switch-apps/SwitchInstaller.ipa'
                    ]
                    self.assertEqual(entry['source'], 'build/switch-apps/SwitchInstaller.ipa')
                    self.assertTrue(entry['bundle'])
                    self.assertTrue(entry['trust'])
                    self.assertEqual(entry['directory'], '/Applications')
                    self.assertEqual(entry['path'], '/Applications/SwitchInstaller.app')

    def test_only_the_kinds_own_sources_are_required(self):
        for kind in self.kinded():
            given = [e['source'] for e in self.manifest['files'] if e.get('kind', kind) == kind]
            self.plan(kind, given)
            mine = next(e['source'] for e in self.released() if e.get('kind') == kind)
            with self.assertRaises(SystemExit):
                self.plan(kind, [s for s in given if s != mine])

    def test_builtin_keeps_its_info_and_icons_in_the_bundle(self):
        source = 'build/switch-apps/SwitchInstaller.ipa'
        path = '/Applications/SwitchInstaller.app'
        for kind in KINDS:
            for release in (False, True):
                with self.subTest(kind=kind, release=release), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    plan = self.plan(kind, release=release)
                    [entry] = [e for e in plan['files'] if e['path'] == path]
                    self.assertEqual(entry['source'], source)
                    self.assertIn(['/Applications', ''], plan['dirs'])
                    info = {
                        'CFBundleIdentifier': 'com.brunostjohn.switch.installer',
                        'CFBundleVersion': '8',
                        'CFBundleExecutable': 'SwitchInstaller',
                        'UIDeviceFamily': [2],
                        'CFBundleIcons~ipad': {'CFBundlePrimaryIcon': {'CFBundleIconFiles': ['AppIcon76x76']}},
                    }
                    resources = {'Assets.car': b'compiled icons', 'AppIcon76x76@2x~ipad.png': b'iPad icon'}
                    ipa = root / 'seed.ipa'
                    with zipfile.ZipFile(ipa, 'w') as archive:
                        archive.writestr('Payload/SwitchInstaller.app/Info.plist', plistlib.dumps(info))
                        for name, data in (resources | {'SwitchInstaller': b'code', 'Other.png': b'unrelated'}).items():
                            archive.writestr('Payload/SwitchInstaller.app/' + name, data)
                    planned = root / 'plan.json'
                    planned.write_text(json.dumps({'files': [entry]}))
                    args = [
                        'overlay',
                        'tree',
                        '--plan',
                        str(planned),
                        '--file',
                        f'{source}={ipa}',
                        '--out',
                        str(root / 'overlay'),
                    ]
                    with mock.patch('sys.argv', args):
                        self.overlay.main()
                    result = root / 'overlay' / path.lstrip('/')
                    self.assertEqual(plistlib.loads((result / 'Info.plist').read_bytes()), info)
                    for name, expected in resources.items():
                        self.assertEqual((result / name).read_bytes(), expected)


if __name__ == '__main__':
    unittest.main()
