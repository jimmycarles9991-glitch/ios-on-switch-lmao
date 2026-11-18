import CoreGraphics
import Foundation

enum SwitchRuntime { static var modern = true }
enum UIUserInterfaceIdiom { case phone, pad }
enum UIUserInterfaceSizeClass { case compact, regular }
final class UIDevice {
    static let current = UIDevice()
    var userInterfaceIdiom = UIUserInterfaceIdiom.pad
}
struct UITraitCollection { var horizontalSizeClass = UIUserInterfaceSizeClass.regular }
struct UIEdgeInsets: Equatable {
    var top: CGFloat
    var left: CGFloat
    var bottom: CGFloat
    var right: CGFloat
    static let zero = UIEdgeInsets(top: 0, left: 0, bottom: 0, right: 0)
}
class UIView {
    var frame = CGRect(x: 0, y: 0, width: 1024, height: 576)
    var bounds: CGRect { frame }
}
final class UITabBar: UIView {}
enum ToolbarPosition { case any }
enum BarMetrics { case `default` }
final class Toolbar {
    var tag = 0
    func setBackgroundImage(_ image: Any?, forToolbarPosition: ToolbarPosition, barMetrics: BarMetrics) {}
}
final class PopupBar {
    enum Style { case prominent }
    enum Progress { case bottom }
    let toolbar = Toolbar()
    var barStyle = Style.prominent
    var progressViewStyle = Progress.bottom
    var tabBarHeight: CGFloat = 0
    var isInlineWithTabBar = false
}
final class PopupContent { var popupCloseButtonAutomaticallyUnobstructsTopBars = true }
final class PopupItem {
    var title = ""
    var subtitle = ""
    var progress: Float = 0
}
final class UINavigationController { let popupItem = PopupItem() }
class UITabBarController {
    enum InteractionStyle { case drag }
    let view = UIView()
    let tabBar = UITabBar()
    let popupBar = PopupBar()
    let popupContentView = PopupContent()
    var traitCollection = UITraitCollection()
    var popupInteractionStyle = InteractionStyle.drag
    var presented = 0
    var dismissed = 0
    var opened = 0
    var closed = 0
    var bottomDockingViewForPopupBar: UIView? { nil }
    var defaultFrameForBottomDockingView: CGRect { .zero }
    var insetsForBottomDockingView: UIEdgeInsets { .zero }
    func presentPopupBar(withContentViewController: UINavigationController, animated: Bool, completion: (() -> Void)?) {
        presented += 1
        completion?()
    }
    func dismissPopupBar(animated: Bool, completion: (() -> Void)?) { dismissed += 1; completion?() }
    func openPopup(animated: Bool, completion: (() -> Void)?) { opened += 1; completion?() }
    func closePopup(animated: Bool, completion: (() -> Void)?) { closed += 1; completion?() }
}
final class DownloadManager {
    static var shared = DownloadManager()
    var lockedForInstallation = false
    var totalProgress = 0.4
    var downloading = 0
    var operations = 0
    var ready = 0
    var uninstalling = 0
    func downloadingPackages() -> Int { downloading }
    func operationCount() -> Int { operations }
    func readyPackages() -> Int { ready }
    func uninstallingPackages() -> Int { uninstalling }
}
extension String { init(localizationKey: String) { self = localizationKey } }
let WHITE_BLUR_TAG = 1
final class TabBarController: UITabBarController {
    var downloadsController: UINavigationController? = UINavigationController()
    var popupIsPresented = false
    var popupLock = DispatchSemaphore(value: 1)
    func updateSileoColors() {}
    func sourceMethods() {}
}

func configuration(modern: Bool, pad: Bool, regular: Bool, width: CGFloat = 1024) -> TabBarController {
    SwitchRuntime.modern = modern
    UIDevice.current.userInterfaceIdiom = pad ? .pad : .phone
    let controller = TabBarController()
    controller.traitCollection.horizontalSizeClass = regular ? .regular : .compact
    controller.view.frame.size.width = width
    controller.tabBar.frame = CGRect(x: 200, y: 32, width: 400, height: 49)
    return controller
}

let modern = configuration(modern: true, pad: true, regular: true)
DownloadManager.shared = DownloadManager()
var completions = 0
modern.updatePopup { completions += 1 }
precondition(completions == 1 && !modern.popupIsPresented && modern.presented == 0)
precondition(modern.bottomDockingViewForPopupBar == nil)
precondition(modern.insetsForBottomDockingView == .zero && modern.defaultFrameForBottomDockingView == .zero)

for mode in 0..<5 {
    let controller = configuration(modern: true, pad: true, regular: true)
    let manager = DownloadManager()
    switch mode {
    case 0: manager.lockedForInstallation = true
    case 1: manager.downloading = 2
    case 2: manager.operations = 3
    case 3: manager.ready = 4
    default: manager.uninstalling = 5
    }
    DownloadManager.shared = manager
    completions = 0
    controller.updatePopup { completions += 1 }
    precondition(controller.popupIsPresented && controller.presented == 1 && completions == 1)
    precondition(!controller.popupBar.isInlineWithTabBar && controller.popupInteractionStyle == .drag)
    precondition(controller.bottomDockingViewForPopupBar == nil && controller.insetsForBottomDockingView == .zero)
    let titles = [
        "Installing_Package_Status", "Downloading_Package_Status", "Queued_Package_Status", "Ready_Status",
        "Removal_Queued_Package_Status",
    ]
    precondition(controller.downloadsController?.popupItem.title == titles[mode])
    controller.presentPopupController { completions += 1 }
    controller.dismissPopupController { completions += 1 }
    precondition(controller.opened == 1 && controller.closed == 1 && completions == 3)
    manager.lockedForInstallation = false
    manager.downloading = 0; manager.operations = 0; manager.ready = 0; manager.uninstalling = 0
    controller.updatePopup { completions += 1 }
    precondition(!controller.popupIsPresented && controller.dismissed == 1 && completions == 4)
}

let legacy = configuration(modern: false, pad: true, regular: true)
DownloadManager.shared = DownloadManager()
legacy.updatePopup()
precondition(legacy.popupIsPresented && legacy.popupBar.isInlineWithTabBar)
precondition(legacy.bottomDockingViewForPopupBar === legacy.tabBar)
precondition(legacy.insetsForBottomDockingView == UIEdgeInsets(top: 49, left: 704, bottom: 0, right: 0))
precondition(legacy.defaultFrameForBottomDockingView == CGRect(x: 0, y: 527, width: 704, height: 49))
let compact = configuration(modern: true, pad: true, regular: false)
compact.updatePopup()
precondition(compact.bottomDockingViewForPopupBar === compact.tabBar && compact.popupBar.isInlineWithTabBar)
let narrow = configuration(modern: true, pad: true, regular: false, width: 600)
narrow.updatePopup()
precondition(!narrow.popupIsPresented && narrow.insetsForBottomDockingView == .zero)
let phone = configuration(modern: true, pad: false, regular: true)
phone.updatePopup()
precondition(!phone.popupIsPresented && phone.bottomDockingViewForPopupBar === phone.tabBar)
DownloadManager.shared.ready = 1
phone.updatePopup()
precondition(phone.popupIsPresented && !phone.popupBar.isInlineWithTabBar)
let missing = configuration(modern: true, pad: true, regular: true)
missing.downloadsController = nil
completions = 0
missing.presentPopup { completions += 1 }
precondition(completions == 1 && !missing.popupIsPresented)
