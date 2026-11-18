import Foundation

class UIResponder {}
protocol UIWindowSceneDelegate {}
class UIScene {
    enum ActivationState { case foregroundActive, foregroundInactive }
    var activationState = ActivationState.foregroundInactive
    struct ConnectionOptions {
        var urlContexts = Set<UIOpenURLContext>()
        var shortcutItem: UIApplicationShortcutItem?
    }
}
final class UIWindowScene: UIScene {}
final class UISceneSession {}
final class UIWindow {
    var visible = false
    func makeKeyAndVisible() { visible = true }
}
final class UIApplication {
    static let shared = UIApplication()
    var delegate: UIResponder?
    enum OpenURLOptionsKey: Hashable { case openInPlace, sourceApplication, annotation }
}
final class UIApplicationShortcutItem {
    let type: String
    init(_ type: String) { self.type = type }
}
final class UIOpenURLContext: Hashable {
    struct Options {
        var openInPlace = true
        var sourceApplication: String? = "org.example.sender"
        var annotation: Any? = "document"
    }
    let url: URL
    let options = Options()
    init(_ url: String) { self.url = URL(string: url)! }
    static func == (lhs: UIOpenURLContext, rhs: UIOpenURLContext) -> Bool { lhs === rhs }
    func hash(into hasher: inout Hasher) { hasher.combine(ObjectIdentifier(self)) }
}
class UIViewController {
    func present(_ controller: UIAlertController, animated: Bool, completion: (() -> Void)?) {}
}
class UINavigationController: UIViewController { var viewControllers = [UIViewController]() }
final class SileoNavigationController: UINavigationController {}
final class SourcesSplitViewController: UIViewController { var viewControllers = [UIViewController]() }
final class SourcesViewController: UIViewController {
    var refreshes = 0
    var additions = 0
    func refreshSources(
        forceUpdate: Bool, forceReload: Bool, isBackground: Bool,
        useRefreshControl: Bool, useErrorScreen: Bool, completion: ((Int, Int) -> Void)?
    ) {
        refreshes += 1
        completion?(0, 0)
    }
    func addSource(_ sender: Any?) { additions += 1 }
}
final class PackageListViewController: UIViewController {}
final class TabBarController: UIViewController {
    static var singleton: TabBarController?
    var viewControllers: [UIViewController]?
    var selectedViewController: UIViewController?
    var popupPresentations = 0
    func presentPopupController() { popupPresentations += 1 }
}
final class UIAlertController: UIViewController {
    enum Style { case alert }
    init(title: String, message: String, preferredStyle: Style) {}
    func dismiss(animated: Bool, completion: (() -> Void)?) { completion?() }
}
final class PackageListManager {
    static let shared = PackageListManager()
    var upgrades = 0
    func upgradeAll(completion: () -> Void) { upgrades += 1; completion() }
}
final class DownloadManager {
    static let shared = DownloadManager()
    func reloadData(recheckPackages: Bool) {}
}
extension String { init(localizationKey: String) { self = localizationKey } }
extension UserDefaults {
    func bool(forKey: String, fallback: Bool) -> Bool { object(forKey: forKey) as? Bool ?? fallback }
}
enum UIColor { static var isTransitionLockedForiOS13Bug = false }
final class SileoAppDelegate: UIResponder {
    var window: UIWindow?
    var events = [String]()
    var scheduledTasks = 0
    func connectWindow(_ window: UIWindow) { self.window = window; events.append("connect") }
    func scheduleTasks() { scheduledTasks += 1 }
    func application(
        _ application: UIApplication, open url: URL,
        options: [UIApplication.OpenURLOptionsKey: Any]
    ) -> Bool {
        precondition(window?.visible == true)
        precondition(options[.openInPlace] as? Bool == true)
        precondition(options[.sourceApplication] as? String == "org.example.sender")
        precondition(options[.annotation] as? String == "document")
        events.append(url.absoluteString)
        return true
    }
}

let app = SileoAppDelegate()
UIApplication.shared.delegate = app
let scene = UIWindowScene()
let session = UISceneSession()
let delegate = SwitchSceneDelegate()
let coldURL = UIOpenURLContext("sileo://source/example")
let coldAction = UIApplicationShortcutItem("Sileo.Packages")
var options = UIScene.ConnectionOptions()
options.urlContexts = [coldURL]
options.shortcutItem = coldAction
delegate.scene(scene, willConnectTo: session, options: options)
precondition(app.events.isEmpty)
let window = UIWindow()
delegate.window = window
delegate.scene(UIScene(), willConnectTo: session, options: options)
precondition(app.events.isEmpty)
delegate.scene(scene, willConnectTo: session, options: options)
precondition(app.events == ["connect"] && app.window === window && window.visible)

var completions = [Bool]()
app.application(UIApplication.shared, performActionFor: coldAction) { completions.append($0) }
precondition(completions == [false])
let tabs = TabBarController()
let sources = SourcesViewController()
let sourcesNavigation = SileoNavigationController()
sourcesNavigation.viewControllers = [sources]
let split = SourcesSplitViewController()
split.viewControllers = [sourcesNavigation]
let packageNavigation = SileoNavigationController()
packageNavigation.viewControllers = [PackageListViewController()]
tabs.viewControllers = [UIViewController(), UIViewController(), split, packageNavigation]
TabBarController.singleton = tabs

scene.activationState = .foregroundActive
delegate.sceneDidBecomeActive(scene)
precondition(app.events == ["connect", coldURL.url.absoluteString])
precondition(tabs.selectedViewController === packageNavigation)
tabs.selectedViewController = nil
delegate.sceneDidBecomeActive(scene)
precondition(app.events.count == 2 && tabs.selectedViewController == nil)
let warmURL = UIOpenURLContext("file:///package.deb")
delegate.scene(scene, openURLContexts: [warmURL])
precondition(app.events.last == warmURL.url.absoluteString)
scene.activationState = .foregroundInactive
let pendingURL = UIOpenURLContext("file:///sources.list")
delegate.scene(scene, openURLContexts: [pendingURL])
precondition(app.events.count == 3)
scene.activationState = .foregroundActive
delegate.sceneDidBecomeActive(scene)
precondition(app.events.last == pendingURL.url.absoluteString && app.events.count == 4)

for suffix in ["Packages", "Refresh", "AddSource", "UpgradeAll", "Unknown"] {
    completions.removeAll()
    delegate.windowScene(scene, performActionFor: UIApplicationShortcutItem("Sileo." + suffix)) {
        completions.append($0)
    }
    precondition(completions == [suffix != "Unknown"])
}
precondition(sources.refreshes == 2 && sources.additions == 1)
precondition(PackageListManager.shared.upgrades == 1 && tabs.popupPresentations == 1)
delegate.sceneDidEnterBackground(scene)
precondition(UIColor.isTransitionLockedForiOS13Bug && app.scheduledTasks == 1)
delegate.sceneWillEnterForeground(scene)
precondition(!UIColor.isTransitionLockedForiOS13Bug)

let replacementWindow = UIWindow()
app.window = replacementWindow
delegate.sceneDidDisconnect(scene)
precondition(app.window === replacementWindow)
app.window = window
scene.activationState = .foregroundInactive
delegate.scene(scene, openURLContexts: [UIOpenURLContext("sileo://discarded")])
delegate.sceneDidDisconnect(scene)
precondition(app.window == nil)
app.window = window
delegate.sceneDidBecomeActive(scene)
precondition(app.events.count == 4)
