import os
import re
from pathlib import Path

from apps.sileo import sileo_scene, sileo_ui
from bootstrap.xcode import replace_once


def adapt_sileo(root: Path) -> None:
    sileo_scene.adapt(root)
    sileo_ui.adapt(root)
    wrapper = root / 'Sileo/Backend/Root Wrapper'
    replace_once(
        root / 'Sileo/Backend/C Contrib/dpkgversion.h',
        '#include <stdio.h>',
        '#include <stdio.h>\n#include <stdint.h>\n#include <sys/types.h>',
    )
    replace_once(
        root / 'Sileo/Backend/C Contrib/dpkgversion.c', 'strrchr(".-+~:", character)', 'strchr(".-+~:", character)'
    )
    replace_once(
        wrapper / 'Bootstrap.swift',
        'case .odysseyra1n, .taurine,',
        'case .switchPlatform, .odysseyra1n, .taurine,',
    )
    replace_once(
        wrapper / 'Jailbreak.swift',
        'enum Jailbreak: String, Codable {',
        'enum Jailbreak: String, Codable {\n    case switchPlatform = "Switch"',
    )
    replace_once(
        wrapper / 'Jailbreak.swift', '.chimera, .odyssey, .taurine,', '.switchPlatform, .chimera, .odyssey, .taurine,'
    )
    replace_once(
        wrapper / 'Jailbreak.swift',
        '        let checkra1n = URL(fileURLWithPath:',
        '        if URL(fileURLWithPath: "/var/jb/.switch_strapped").exists {\n'
        '            self = .switchPlatform\n            return\n        }\n\n'
        '        let checkra1n = URL(fileURLWithPath:',
    )
    adapt_sileo_respring(root / 'Sileo/UI/DownloadsViewController/DownloadsTableViewController.swift')


def adapt_sileo_respring(path: Path) -> None:
    command = r'spawn(command: "\(CommandPath.prefix)/usr/bin/sbreload", args: ["sbreload"])'
    replace_once(
        path,
        '                ' + command,
        r"""                if Jailbreak.current == .switchPlatform {
                    DispatchQueue.global(qos: .userInitiated).async {
                        let (status, _, errors) = spawn(
                            command: "\(CommandPath.prefix)/usr/bin/sbreload", args: ["sbreload"])
                        DispatchQueue.main.async {
                            if status == 0 { exit(0) }
                            window.alpha = 1
                            window.transform = .identity
                            self.isFired = false
                            self.setNeedsStatusBarAppearanceUpdate()
                            let message = errors.isEmpty ? "SpringBoard restart failed (\(status))." : errors
                            let alert = UIAlertController(title: String(localizationKey: "Error"),
                                                          message: message, preferredStyle: .alert)
                            alert.addAction(UIAlertAction(title: String(localizationKey: "OK"), style: .default))
                            self.present(alert, animated: true)
                        }
                    }
                    return
                }
                """
        + command,
    )


def adapt_dependencies(dependencies: dict[str, Path]) -> None:
    picker = dependencies['alderis'] / 'Alderis/ColorPickerInnerViewController.swift'
    replace_once(picker, 'var tab: ColorPickerTab', 'var selectedPickerTab: ColorPickerTab')
    replace_once(picker, 'tab = configuration.initialTab', 'selectedPickerTab = configuration.initialTab')


def localize_packages(root: Path, dependencies: dict[str, Path]) -> None:
    project = root / 'Sileo.xcodeproj/project.pbxproj'
    text = project.read_text()
    pattern = re.compile(
        r'isa = XCRemoteSwiftPackageReference;\s*repositoryURL = "([^"]+)";\s*'
        r'requirement = \{[^}]+\};'
    )

    def local_reference(match: re.Match[str]) -> str:
        name = match[1].removesuffix('.git').rsplit('/', 1)[-1].lower()
        path = dependencies[name]
        relative = os.path.relpath(path, root)
        return f'isa = XCLocalSwiftPackageReference;\n\t\t\trelativePath = "{relative}";'

    text, count = pattern.subn(local_reference, text)
    if count != 16 or 'isa = XCRemoteSwiftPackageReference;' in text:
        raise ValueError('Sileo Swift package references differ from the source pin')
    project.write_text(text)
    (root / 'Sileo.xcodeproj/project.xcworkspace/xcshareddata/swiftpm/Package.resolved').unlink()
    for dependency in dependencies.values():
        manifest = dependency / 'Package.swift'
        if not manifest.exists():
            continue
        manifest.write_text(local_manifest(manifest.read_text(), dependency, dependencies))


def local_manifest(text: str, dependency: Path, dependencies: dict[str, Path]) -> str:
    pattern = re.compile(r'\.package\((?:[^()]|\([^()]*\))*\)', re.DOTALL)

    def local_dependency(match: re.Match[str]) -> str:
        remote = re.search(r'url:\s*"([^"]+)"', match[0])
        if remote is None:
            return match[0]
        name = remote[1].removesuffix('.git').rsplit('/', 1)[-1].lower()
        if name == 'csqlite':
            # SQLite only declares this dependency inside its Linux build branch.
            return match[0]
        if name not in dependencies:
            raise ValueError(f'unpinned Swift dependency: {remote[1]}')
        relative = os.path.relpath(dependencies[name], dependency)
        return f'.package(path: "{relative}")'

    text = pattern.sub(local_dependency, text)
    if dependency.name == 'zippyjson':
        before = 'dependencies: ["ZippyJSONCFamily", "JJLISO8601DateFormatter"]'
        after = (
            'dependencies: [.product(name: "ZippyJSONCFamily", package: "zippyjsoncfamily"), '
            '.product(name: "JJLISO8601DateFormatter", package: "jjliso8601dateformatter")]'
        )
        if text.count(before) != 1:
            raise ValueError('ZippyJSON dependency declarations differ from the source pin')
        text = text.replace(before, after)
    return text
