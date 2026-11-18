final class SwitchSceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    private var pendingURLs = Set<UIOpenURLContext>()
    private var pendingShortcut: UIApplicationShortcutItem?

    private var appDelegate: SileoAppDelegate {
        UIApplication.shared.delegate as! SileoAppDelegate
    }

    func scene(
        _ scene: UIScene, willConnectTo session: UISceneSession,
        options connectionOptions: UIScene.ConnectionOptions
    ) {
        guard scene is UIWindowScene, let window = window else { return }
        appDelegate.connectWindow(window)
        window.makeKeyAndVisible()
        pendingURLs = connectionOptions.urlContexts
        pendingShortcut = connectionOptions.shortcutItem
    }

    func sceneDidBecomeActive(_ scene: UIScene) {
        let urls = pendingURLs
        pendingURLs.removeAll()
        open(urls)
        if let shortcut = pendingShortcut {
            pendingShortcut = nil
            appDelegate.application(UIApplication.shared, performActionFor: shortcut) { _ in }
        }
    }

    func scene(_ scene: UIScene, openURLContexts URLContexts: Set<UIOpenURLContext>) {
        if scene.activationState == .foregroundActive {
            open(URLContexts)
        } else {
            pendingURLs.formUnion(URLContexts)
        }
    }

    private func open(_ contexts: Set<UIOpenURLContext>) {
        for context in contexts {
            var options: [UIApplication.OpenURLOptionsKey: Any] = [.openInPlace: context.options.openInPlace]
            if let source = context.options.sourceApplication { options[.sourceApplication] = source }
            if let annotation = context.options.annotation { options[.annotation] = annotation }
            _ = appDelegate.application(UIApplication.shared, open: context.url, options: options)
        }
    }

    func windowScene(
        _ windowScene: UIWindowScene, performActionFor shortcutItem: UIApplicationShortcutItem,
        completionHandler: @escaping (Bool) -> Void
    ) {
        appDelegate.application(
            UIApplication.shared, performActionFor: shortcutItem, completionHandler: completionHandler)
    }

    func sceneWillEnterForeground(_ scene: UIScene) {
        appDelegate.applicationWillEnterForeground(UIApplication.shared)
    }

    func sceneDidEnterBackground(_ scene: UIScene) {
        appDelegate.applicationDidEnterBackground(UIApplication.shared)
    }

    func sceneDidDisconnect(_ scene: UIScene) {
        if appDelegate.window === window { appDelegate.window = nil }
        pendingURLs.removeAll()
        pendingShortcut = nil
    }
}
