import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from apps.sileo.adapt import adapt_sileo_respring


class SileoRespringTests(unittest.TestCase):
    def test_actual_sileo_respring_completion(self):
        path = 'Sileo/UI/DownloadsViewController/DownloadsTableViewController.swift'
        pin = next(Path(os.environ['TEST_SRCDIR']).glob(f'*bootstrap_sileo/{path}'))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            controller = root / 'DownloadsTableViewController.swift'
            shutil.copyfile(pin, controller)
            adapt_sileo_respring(controller)
            text = controller.read_text()
            start = text.index('                if Jailbreak.current == .switchPlatform')
            end = text.index('\n                spawn(command:', start)
            program = r"""
import Darwin

enum Jailbreak { case switchPlatform; static let current = Jailbreak.switchPlatform }
enum CommandPath { static let prefix = "/var/jb" }
struct DispatchQueue {
    enum QoS { case userInitiated }
    static let main = DispatchQueue()
    static func global(qos: QoS) -> DispatchQueue { return main }
    func async(_ work: () -> Void) { work() }
}
extension String {
    init(localizationKey: String) { self = localizationKey }
}
struct UIAlertAction {
    enum Style { case `default` }
    init(title: String, style: Style) { precondition(title == "OK") }
}
struct UIAlertController {
    enum Style { case alert }
    let message: String
    init(title: String, message: String, preferredStyle: Style) {
        precondition(title == "Error")
        self.message = message
    }
    func addAction(_ action: UIAlertAction) {}
}
final class Window {
    enum Transform { case scaled, identity }
    var alpha: Double = 0
    var transform = Transform.scaled
}
func spawn(command: String, args: [String]) -> (Int, String, String) {
    precondition(command == "/var/jb/usr/bin/sbreload" && args == ["sbreload"])
    let status = Int(CommandLine.arguments[1])!
    return (status, "", CommandLine.arguments[2])
}
final class Controller {
    var isFired = true
    var refreshed = false
    var message: String?
    let window = Window()
    func setNeedsStatusBarAppearanceUpdate() { refreshed = true }
    func present(_ alert: UIAlertController, animated: Bool) { message = alert.message }
    func complete() {
"""
            program += (
                text[start:end]
                + """
        preconditionFailure("unexpected platform")
    }
}
let controller = Controller()
controller.complete()
precondition(CommandLine.arguments[1] != "0")
precondition(!controller.isFired && controller.refreshed && controller.window.alpha == 1)
precondition(controller.window.transform == .identity)
precondition(controller.message == CommandLine.arguments[3])
"""
            )
            (root / 'main.swift').write_text(program)
            executable = str(root / 'respring')
            subprocess.run(
                ['/usr/bin/xcrun', '--sdk', 'macosx', 'swiftc', str(root / 'main.swift'), '-o', executable],
                check=True,
                capture_output=True,
            )
            for arguments in (
                ('0', '', ''),
                ('1', 'launchd refused restart', 'launchd refused restart'),
                ('1', '', 'SpringBoard restart failed (1).'),
            ):
                subprocess.run([executable, *arguments], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
