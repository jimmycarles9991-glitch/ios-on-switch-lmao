import os
import plistlib
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from apps.sileo import sileo_scene


class SileoSceneTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.pin = next(Path(os.environ['TEST_SRCDIR']).glob('*bootstrap_sileo/Sileo'))
        (self.root / 'Sileo').mkdir()
        for name in ('AppDelegate.swift', 'Info.plist'):
            shutil.copyfile(self.pin / name, self.root / 'Sileo' / name)

    def adapted(self):
        sileo_scene.adapt(self.root)
        return (self.root / 'Sileo/AppDelegate.swift').read_text()

    def test_native_single_storyboard_scene_preserves_other_metadata(self):
        original = plistlib.loads((self.pin / 'Info.plist').read_bytes())
        self.adapted()
        info = plistlib.loads((self.root / 'Sileo/Info.plist').read_bytes())
        manifest = info.pop('UIApplicationSceneManifest')
        del original['UIMainStoryboardFile']
        self.assertEqual(info, original)
        self.assertIs(manifest['UIApplicationSupportsMultipleScenes'], False)
        self.assertEqual(
            manifest['UISceneConfigurations'],
            {
                'UIWindowSceneSessionRoleApplication': [
                    {
                        'UISceneConfigurationName': 'Sileo',
                        'UISceneDelegateClassName': '$(PRODUCT_MODULE_NAME).SwitchSceneDelegate',
                        'UISceneStoryboardFile': 'Main',
                    }
                ]
            },
        )
        storyboard = ET.parse(self.pin / 'Base.lproj/Main.storyboard').getroot()
        controller = storyboard.find(f".//*[@id='{storyboard.attrib['initialViewController']}']")
        self.assertEqual(controller.attrib['customClass'], 'TabBarController')

    def test_launch_registers_background_tasks_before_any_window(self):
        text = self.adapted()
        launch = text[text.index('    func application(') : text.index('    func connectWindow(')]
        connection = text[
            text.index('    func connectWindow(') : text.index('    private func backgroundRepoRefreshTask')
        ]
        for operation in (
            'Evander.prepare()',
            '_ = RepoManager.shared',
            '_ = PackageListManager.shared',
            '_ = DatabaseManager.shared',
            '_ = DownloadManager.shared',
            '_ = LanguageHelper.shared',
            'BGTaskScheduler.shared.register(',
            'UNUserNotificationCenter.current().requestAuthorization',
            'NotificationCenter.default.addObserver(',
        ):
            self.assertEqual(launch.count(operation), 1)
            self.assertNotIn(operation, connection)
        self.assertIn('didFinishLaunchingWithOptions', launch)
        self.assertIn('return true', launch)
        self.assertNotIn('rootViewController', launch)
        self.assertNotIn('applicationDidFinishLaunching', text)
        self.assertLess(connection.index('self.window = window'), connection.index('rootViewController'))
        self.assertIn('tabBarController.delegate = self', connection)
        self.assertIn('(tabBarController.viewControllers ?? []).enumerated()', connection)
        self.assertIn('Search_Page', connection)
        self.assertIn('SileoThemeManager.shared.updateUserInterface()', connection)

    def test_source_drift_fails_instead_of_silently_skipping_migration(self):
        path = self.root / 'Sileo/AppDelegate.swift'
        path.write_text(path.read_text().replace('func applicationDidFinishLaunching(', 'func changedLaunch('))
        with self.assertRaisesRegex(ValueError, 'source pin'):
            sileo_scene.adapt(self.root)

    def test_actual_scene_routes_cold_and_warm_events_and_completes_shortcuts(self):
        text = self.adapted()
        actions = text[
            text.index(
                '    func application(_ application: UIApplication, performActionFor shortcutItem:'
            ) : text.index('    @available(iOS 13.0, *)\n    private func handleRefreshTask')
        ]
        fixture = Path(__file__).with_name('sileo_scene_test.swift').read_text()
        fixture = fixture.replace('    func scheduleTasks()', actions + '\n    func scheduleTasks()')
        fixture += '\n' + text[text.index('final class SwitchSceneDelegate:') :]
        source = self.root / 'main.swift'
        source.write_text(fixture)
        executable = str(self.root / 'scene')
        result = subprocess.run(
            ['/usr/bin/xcrun', '--sdk', 'macosx', 'swiftc', str(source), '-o', executable],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
