import CoreGraphics
import Foundation

struct Insets { var top: CGFloat = 0 }
final class View { var safeAreaInsets = Insets() }
final class Constraint { var constant: CGFloat = 200 }
final class UIScrollView {
    var adjustedContentInset = Insets()
    var contentOffset = CGPoint.zero
    var target: CGPoint?
    func setContentOffset(_ point: CGPoint, animated: Bool) {
        precondition(animated)
        target = point
    }
}
final class PackageGeometry {
    let view = View()
    let depictionHeaderImageViewHeight = Constraint()
    // SWITCH_PACKAGE_SNAP
}
final class NativeGeometry {
    let view = View()
    let headerImageViewHeightAnchor = Constraint()
    // SWITCH_NATIVE_SNAP
}

func verifyGeometry(view: View, header: Constraint, snap: (UIScrollView, Bool) -> Void) {
    let scroll = UIScrollView()
    view.safeAreaInsets.top = 64
    scroll.contentOffset = CGPoint(x: 9, y: 30)
    snap(scroll, false)
    precondition(scroll.target == CGPoint(x: 9, y: 0))
    scroll.contentOffset.y = 100
    snap(scroll, true)
    precondition(scroll.target == CGPoint(x: 9, y: 136))
    view.safeAreaInsets.top = 140
    scroll.contentOffset.y = 40
    snap(scroll, true)
    precondition(scroll.target == CGPoint(x: 9, y: 60))
    scroll.adjustedContentInset.top = 20
    scroll.contentOffset.y = 0
    snap(scroll, false)
    precondition(scroll.target == CGPoint(x: 9, y: -20))
    scroll.target = nil
    scroll.contentOffset.y = -25
    snap(scroll, true)
    precondition(scroll.target == nil)
    scroll.contentOffset.y = 80
    snap(scroll, true)
    precondition(scroll.target == nil)
    header.constant = 0
    scroll.contentOffset.y = 1
    snap(scroll, false)
    precondition(scroll.target == nil)
}
let package = PackageGeometry()
verifyGeometry(view: package.view, header: package.depictionHeaderImageViewHeight,
               snap: package.scrollViewDidEndDragging)
let native = NativeGeometry()
verifyGeometry(view: native.view, header: native.headerImageViewHeightAnchor,
               snap: native.scrollViewDidEndDragging)

enum Idiom { case phone, pad }
enum SizeClass { case regular, compact }
final class UIDevice {
    static let current = UIDevice()
    var userInterfaceIdiom = Idiom.pad
}
final class UITraitCollection {
    let horizontalSizeClass: SizeClass
    init(horizontalSizeClass: SizeClass) { self.horizontalSizeClass = horizontalSizeClass }
    init(traitsFrom traits: [UITraitCollection]) { horizontalSizeClass = traits.last!.horizontalSizeClass }
}
final class Orientation { var isLandscape = false }
final class Scene { let interfaceOrientation = Orientation() }
final class Window { let windowScene: Scene? = Scene() }
class UITabBar {
    let native = UITraitCollection(horizontalSizeClass: .regular)
    var traitCollection: UITraitCollection { native }
    var window: Window? = Window()
    var bounds = CGRect(x: 0, y: 0, width: 600, height: 50)
}
var switchModernUI = true
// SWITCH_TAB_CLASS

let tab = TabBar()
precondition(tab.traitCollection === tab.native)
switchModernUI = false
precondition(tab.traitCollection.horizontalSizeClass == .compact)
tab.window?.windowScene?.interfaceOrientation.isLandscape = true
precondition(tab.traitCollection === tab.native)
tab.bounds.size.width = 300
precondition(tab.traitCollection.horizontalSizeClass == .compact)
UIDevice.current.userInterfaceIdiom = .phone
precondition(tab.traitCollection === tab.native)
