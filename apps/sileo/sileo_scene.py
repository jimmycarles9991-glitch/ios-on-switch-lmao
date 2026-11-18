import plistlib
from pathlib import Path


def adapt(root: Path) -> None:
    delegate = root / 'Sileo/AppDelegate.swift'
    source = delegate.read_text()

    def replace(before: str, after: str) -> None:
        nonlocal source
        if source.count(before) != 1:
            raise ValueError('Sileo scene lifecycle differs from the source pin')
        source = source.replace(before, after)

    replace(
        '    func applicationDidFinishLaunching(_ application: UIApplication) {',
        '    func application(\n'
        '        _ application: UIApplication,\n'
        '        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?\n'
        '    ) -> Bool {',
    )
    start = '        guard let tabBarController = self.window?.rootViewController as? UITabBarController else {'
    end = '        if #available(iOS 13.0, *) {\n            BGTaskScheduler.shared.register'
    if source.count(end) != 1 or source.count(start) != 1:
        raise ValueError('Sileo storyboard initialization differs from the source pin')
    window_setup = source[source.index(start) : source.index(end)]
    replace(window_setup, '')
    start = '        // Force all view controllers to load now\n'
    end = '    \n    private func backgroundRepoRefreshTask'
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError('Sileo tab initialization differs from the source pin')
    tabs = source[source.index(start) : source.index(end)]
    replace(
        tabs,
        '        return true\n    }\n\n'
        '    func connectWindow(_ window: UIWindow) {\n'
        '        self.window = window\n' + window_setup + '        SileoThemeManager.shared.updateUserInterface()\n'
        '        updateTintColor()\n' + tabs,
    )
    replace(
        '        else {\n            return\n        }\n'
        '        \n        if shortcutItem.type.hasSuffix(".UpgradeAll") {',
        '        else {\n            completionHandler(false)\n            return\n        }\n'
        '        \n        if shortcutItem.type.hasSuffix(".UpgradeAll") {',
    )
    replace(
        '        } else if shortcutItem.type.hasSuffix(".Packages") {\n'
        '            tabBarController.selectedViewController = packageListNVC\n        }\n    }',
        '        } else if shortcutItem.type.hasSuffix(".Packages") {\n'
        '            tabBarController.selectedViewController = packageListNVC\n'
        '        } else {\n            completionHandler(false)\n            return\n        }\n'
        '        completionHandler(true)\n    }',
    )
    scene_delegate = Path(__file__).with_name('SwitchSceneDelegate.swift').read_text()
    delegate.write_text(source.rstrip() + '\n\n' + scene_delegate)

    info_path = root / 'Sileo/Info.plist'
    info = plistlib.loads(info_path.read_bytes())
    if info.get('UIMainStoryboardFile') != 'Main' or 'UIApplicationSceneManifest' in info:
        raise ValueError('Sileo storyboard configuration differs from the source pin')
    del info['UIMainStoryboardFile']
    info['UIApplicationSceneManifest'] = {
        'UIApplicationSupportsMultipleScenes': False,
        'UISceneConfigurations': {
            'UIWindowSceneSessionRoleApplication': [
                {
                    'UISceneConfigurationName': 'Sileo',
                    'UISceneDelegateClassName': '$(PRODUCT_MODULE_NAME).SwitchSceneDelegate',
                    'UISceneStoryboardFile': 'Main',
                }
            ]
        },
    }
    info_path.write_bytes(plistlib.dumps(info, sort_keys=False))
