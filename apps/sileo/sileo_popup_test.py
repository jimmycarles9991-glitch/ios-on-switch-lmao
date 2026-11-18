import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from apps.sileo import sileo_popup


def method(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class SileoPopupTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        runfiles = Path(os.environ['TEST_SRCDIR'])
        self.pin = next(runfiles.glob('*bootstrap_sileo/Sileo/UI/TabBarController/TabBarController.swift'))
        self.dependency = next(runfiles.glob('*bootstrap_sileo_lnpopupcontroller/LNPopupController/LNPopupController'))
        self.path = self.root / 'Sileo/UI/TabBarController/TabBarController.swift'
        self.path.parent.mkdir(parents=True)
        shutil.copyfile(self.pin, self.path)

    def test_actual_queue_modes_preserve_transactions_and_legacy_layout(self):
        sileo_popup.adapt(self.root)
        source = self.path.read_text()
        policy = method(source, '    private var switchUsesScenePopupDock: Bool')
        policy = policy.replace('if #available(iOS 27.0, *)', 'if SwitchRuntime.modern')
        popup = source[
            source.index('    func presentPopup()') : source.index('    override func traitCollectionDidChange')
        ]
        fixture = Path(__file__).with_name('sileo_popup_test.swift').read_text()
        fixture = fixture.replace(method(fixture, '    func sourceMethods()'), policy + '\n' + popup)
        program = self.root / 'main.swift'
        program.write_text(fixture)
        executable = str(self.root / 'popup')
        result = subprocess.run(
            ['/usr/bin/xcrun', '--sdk', 'macosx', 'swiftc', str(program), '-o', executable],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True, capture_output=True)

    def test_actual_dependency_nil_dock_uses_full_width_and_bottom_safe_area(self):
        controller = (self.dependency / 'Private/LNPopupController.m').read_text()
        support = (self.dependency / 'Private/UIViewController+LNPopupSupport.m').read_text()
        popup = '\n'.join(
            method(controller, signature)
            for signature in (
                '- (CGRect)_frameForOpenPopupBar',
                '- (CGRect)_frameForClosedPopupBar',
            )
        )
        container = '\n'.join(
            method(support, signature)
            for signature in (
                '- (CGRect)defaultFrameForBottomDockingView_internal\n',
                '- (CGRect)defaultFrameForBottomDockingView_internalOrDeveloper',
            )
        )
        fixture = Path(__file__).with_name('sileo_popup_geometry_test.m').read_text()
        fixture = fixture.replace(method(fixture, '- (void)containerMethods'), container)
        fixture = fixture.replace(method(fixture, '- (void)popupMethods'), popup)
        program = self.root / 'geometry.m'
        program.write_text(fixture)
        executable = str(self.root / 'geometry')
        result = subprocess.run(
            [
                '/usr/bin/xcrun',
                '--sdk',
                'macosx',
                'clang',
                '-fobjc-arc',
                '-framework',
                'Foundation',
                '-framework',
                'CoreGraphics',
                str(program),
                '-o',
                executable,
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([executable], check=True, capture_output=True)

    def test_layout_pin_drift_is_rejected(self):
        self.path.write_text(
            self.path.read_text().replace('self.view.frame.width >= 768', 'self.view.frame.width >= 800')
        )
        with self.assertRaisesRegex(ValueError, 'source pin'):
            sileo_popup.adapt(self.root)


if __name__ == '__main__':
    unittest.main()
