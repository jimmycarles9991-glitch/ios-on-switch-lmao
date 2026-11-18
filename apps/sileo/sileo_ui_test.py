import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from apps.sileo import sileo_scene, sileo_ui


class SileoUITests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.pin = next(Path(os.environ['TEST_SRCDIR']).glob('*bootstrap_sileo/Sileo'))
        for relative in (
            'AppDelegate.swift',
            'Info.plist',
            'Sileo-Bridging-Header.h',
            'UI/FeaturedViewController/FeaturedViewController.swift',
            'UI/PackageListViewController/PackageListViewController.swift',
            'UI/SettingsViewController/Root/SettingsIconHeaderView.swift',
            'UI/SourcesViewController/SourcesViewController.swift',
            'UI/Utils/PackageIconView.swift',
            'UI/NewsViewController/NewsViewController.swift',
            'UI/PackageListViewController/ListHeader/PackageListHeader.swift',
            'UI/PackageViewController/DepictionView/DepictionScreenshotsView/DepictionScreenshotsViewController/DepictionScreenshotsViewController.swift',
            'UI/PackageViewController/PackageViewController.swift',
            'UI/NativePackageViewController/NativePackageViewController.swift',
            'UI/SourcesViewController/SourcesSplitViewController.swift',
            'UI/SettingsViewController/Root/SettingsViewController.swift',
            'UI/SettingsViewController/Themes/ThemePickerView.swift',
            'UI/WhiteBlur/WhiteBlur.h',
            'UI/WhiteBlur/WhiteBlur.m',
            'UI/TabBarController/TabBar.swift',
            'UI/TabBarController/TabBarController.swift',
        ):
            target = self.root / 'Sileo' / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.pin / relative, target)

    def test_scene_and_public_tab_configuration_compose(self):
        sileo_scene.adapt(self.root)
        sileo_ui.adapt(self.root)
        delegate = (self.root / 'Sileo/AppDelegate.swift').read_text()
        self.assertIn('func connectWindow(', delegate)
        self.assertIn('tabBar.isTranslucent = true', delegate)
        self.assertNotIn('tabBar.scrollEdgeAppearance =', delegate)
        self.assertNotIn('UITabBarAppearance()', delegate)
        self.assertIn('tabBarItem.title = String(localizationKey: "Search_Page")', delegate)
        self.assertNotIn('_blurEnabled', delegate)
        self.assertNotIn('_setInternalTitle', delegate)
        bridge = (self.root / 'Sileo/Sileo-Bridging-Header.h').read_text()
        self.assertNotIn('@interface UITabBarItem (Private)', bridge)
        self.assertNotIn('@interface UITabBar (Private)', bridge)

    def test_public_search_and_corner_apis_preserve_package_badges(self):
        sileo_ui.adapt(self.root)
        relative = 'UI/PackageListViewController/PackageListViewController.swift'
        original = (self.pin / relative).read_text()
        text = (self.root / 'Sileo' / relative).read_text()
        self.assertNotIn('value(forKey: "searchField")', text)
        self.assertNotIn('value(forKey: "_searchField")', text)
        self.assertEqual(text.count('.searchBar.searchTextField'), 5)
        self.assertEqual(
            [line for line in text.splitlines() if 'tabBarItem.badgeValue' in line],
            [line for line in original.splitlines() if 'tabBarItem.badgeValue' in line],
        )
        for relative in (
            'UI/Utils/PackageIconView.swift',
            'UI/SettingsViewController/Root/SettingsIconHeaderView.swift',
        ):
            text = (self.root / 'Sileo' / relative).read_text()
            self.assertIn('.layer.cornerCurve = .continuous', text)
            self.assertNotIn('forKey: "continuousCorners"', text)

    def test_pinned_badge_source_drift_is_rejected(self):
        path = self.root / 'Sileo/UI/SourcesViewController/SourcesViewController.swift'
        path.write_text(path.read_text().replace('forKey: "_badge"', 'forKey: "changed"', 1))
        with self.assertRaisesRegex(ValueError, 'source pin'):
            sileo_ui.adapt(self.root)

    def test_bars_and_picker_use_public_app_owned_styling(self):
        sileo_ui.adapt(self.root)
        for path in (self.root / 'Sileo').rglob('*.swift'):
            text = path.read_text()
            self.assertNotIn('._hidesShadow', text, str(path))
            self.assertNotIn('._backgroundOpacity', text, str(path))
        helper = (self.root / 'Sileo/UI/WhiteBlur/WhiteBlur.m').read_text()
        self.assertIn('Copyright © 2018 Kabir Oberai', helper)
        self.assertNotIn('hook(', helper)
        self.assertNotIn('_UIVisualEffect', helper)
        self.assertNotIn('filterName', helper)
        self.assertNotIn('UIVisualEffectView', helper)
        self.assertNotIn('UIViewPropertyAnimator', helper)
        self.assertIn('[self setContentScrollView:scrollView forEdge:NSDirectionalRectEdgeTop]', helper)
        self.assertIn('self.navigationItem.scrollEdgeAppearance =', helper)
        self.assertNotIn('self.alpha =', helper)
        picker = (self.root / 'Sileo/UI/SettingsViewController/Themes/ThemePickerView.swift').read_text()
        self.assertIn('attributedTitleForRow', picker)
        self.assertIn(
            'NSAttributedString(string: values[row], attributes: [.foregroundColor: UIColor.sileoLabel])', picker
        )
        self.assertIn('callback?.themeSelected(row)', picker)

    def test_actual_bar_helpers_preserve_appearance_and_native_scroll_binding(self):
        fixture = Path(__file__).with_name('sileo_bars_test.m').read_text()
        header = Path(__file__).with_name('SwitchBarAppearance.h').read_text().replace('#import <UIKit/UIKit.h>', '')
        helper = Path(__file__).with_name('SwitchBarAppearance.m').read_text().replace('#import "WhiteBlur.h"', '')
        fixture = fixture.replace('// SWITCH_BAR_HEADER', header).replace('// SWITCH_BAR_IMPLEMENTATION', helper)
        path = self.root / 'bars.m'
        path.write_text(fixture)
        executable = str(self.root / 'bars')
        result = subprocess.run(
            [
                '/usr/bin/xcrun',
                '--sdk',
                'macosx',
                'clang',
                '-fobjc-arc',
                '-framework',
                'Foundation',
                str(path),
                '-o',
                executable,
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True, capture_output=True)

    def test_actual_header_geometry_and_native_tab_traits(self):
        sileo_ui.adapt(self.root)
        fixture = Path(__file__).with_name('sileo_geometry_test.swift').read_text()
        signature = '    func scrollViewDidEndDragging(_ scrollView: UIScrollView, willDecelerate decelerate: Bool) {'
        for relative, marker in (
            ('UI/PackageViewController/PackageViewController.swift', '// SWITCH_PACKAGE_SNAP'),
            ('UI/NativePackageViewController/NativePackageViewController.swift', '// SWITCH_NATIVE_SNAP'),
        ):
            text = (self.root / 'Sileo' / relative).read_text()
            self.assertNotIn('UIApplication.shared.statusBarFrame', text)
            self.assertNotIn('currentNavBarOpacity', text)
            self.assertIn('switchConfigureScrollEdge(for: scrollView)', text)
            self.assertIn('scrollView.convert(view.safeAreaLayoutGuide.layoutFrame, from: view)', text)
            start = text.index(signature)
            end = text.index('\n    }', start) + len('\n    }')
            fixture = fixture.replace(marker, text[start:end])
        tab = (self.root / 'Sileo/UI/TabBarController/TabBar.swift').read_text()
        self.assertNotIn('UIApplication.shared.statusBarOrientation', tab)
        tab = tab.replace('import Foundation', '').replace('#available(iOS 27.0, *)', 'switchModernUI')
        fixture = fixture.replace('// SWITCH_TAB_CLASS', tab)
        path = self.root / 'geometry.swift'
        path.write_text(fixture)
        executable = str(self.root / 'geometry')
        result = subprocess.run(
            ['/usr/bin/xcrun', '--sdk', 'macosx', 'swiftc', str(path), '-o', executable],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True, capture_output=True)

    def test_actual_refresh_paths_use_public_badges_and_preserve_completion(self):
        sileo_ui.adapt(self.root)
        source = (self.root / 'Sileo/UI/SourcesViewController/SourcesViewController.swift').read_text()
        methods = source[
            source.index('    private func killIndicator() {') : source.index(
                '    func showRefreshErrorViewController('
            )
        ]
        self.assertNotIn('value(forKey:', methods)
        self.assertNotIn('UIActivityIndicatorView', methods)
        fixture = Path(__file__).with_name('sileo_ui_test.swift').read_text()
        self.assertEqual(fixture.count('    // SWITCH_SOURCE_METHODS'), 1)
        fixture = fixture.replace('    // SWITCH_SOURCE_METHODS', methods)
        path = self.root / 'main.swift'
        path.write_text(fixture)
        executable = str(self.root / 'refresh')
        result = subprocess.run(
            ['/usr/bin/xcrun', '--sdk', 'macosx', 'swiftc', str(path), '-o', executable],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
