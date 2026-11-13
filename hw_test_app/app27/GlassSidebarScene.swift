import HWTestKit
import UIKit

/// Settings' sidebar as ui.glass_sidebar builds it: a double-column split view showing the sidebar beside the detail,
/// a sidebar-appearance list of symbol-and-title rows, and a detail of horizontal white and grey stripes (the detail
/// scrolls vertically) in a scroll view as wide as the screen, so it reaches under the floating sidebar wherever UIKit
/// lets the detail column extend. Dark, as Settings ran. With a pane (ui.glass_settings), a Liquid Glass panel stands
/// over the detail, hidden until a phase shows it: Settings' glass bars and buttons over its right column.
@MainActor
final class SidebarScene {
    let host = SidebarHost()
    let split = UISplitViewController(style: .doubleColumn)
    let list: UICollectionView
    let detail = UIScrollView()
    let pane: UIView?
    private let rows = SidebarRows()

    init(pane withPane: Bool = false) {
        let sidebar = UICollectionViewController(
            collectionViewLayout: UICollectionViewCompositionalLayout.list(
                using: UICollectionLayoutListConfiguration(appearance: .sidebar)))
        list = sidebar.collectionView
        list.dataSource = rows
        list.showsVerticalScrollIndicator = false
        let screen = Snapshots.aligned(UIScreen.main.bounds)
        let stripes = UIView(frame: CGRect(x: 0, y: 0, width: screen.width, height: screen.height * 4))
        stripes.backgroundColor = UIColor(white: 0.45, alpha: 1)
        for y in stride(from: CGFloat(0), to: stripes.bounds.height, by: 48) {
            let stripe = UIView(frame: Snapshots.aligned(CGRect(x: 0, y: y, width: screen.width, height: 24)))
            stripe.backgroundColor = .white
            stripes.addSubview(stripe)
        }
        detail.addSubview(stripes)
        detail.contentSize = stripes.bounds.size
        detail.contentInsetAdjustmentBehavior = .never
        detail.showsVerticalScrollIndicator = false
        detail.showsHorizontalScrollIndicator = false
        detail.backgroundColor = stripes.backgroundColor
        let secondary = UIViewController()
        if withPane {
            let column = UIView()
            detail.frame = column.bounds
            detail.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            column.addSubview(detail)
            let glass = UIVisualEffectView(effect: UIGlassEffect(style: .regular))
            glass.isHidden = true
            column.addSubview(glass)
            secondary.view = column
            pane = glass
            split.minimumPrimaryColumnWidth = 200
            split.preferredPrimaryColumnWidthFraction = 0.3
        } else {
            secondary.view = detail
            pane = nil
        }
        split.preferredDisplayMode = .oneBesideSecondary
        split.preferredSplitBehavior = .tile
        split.setViewController(sidebar, for: .primary)
        split.setViewController(secondary, for: .secondary)
        host.addChild(split)
        split.view.frame = host.view.bounds
        split.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        host.view.addSubview(split.view)
        split.didMove(toParent: host)
        host.overrideUserInterfaceStyle = .dark
        host.traitOverrides.horizontalSizeClass = .regular
        host.modalPresentationStyle = .fullScreen
    }

    /// The scene presented over the app without animation; nil when the app has no key window or UIKit refused the
    /// presentation.
    static func present(pane: Bool = false) -> SidebarScene? {
        let roots = UIApplication.shared.connectedScenes.compactMap {
            ($0 as? UIWindowScene)?.keyWindow?.rootViewController
        }
        guard var top = roots.first else { return nil }
        while let presented = top.presentedViewController { top = presented }
        let scene = SidebarScene(pane: pane)
        top.present(scene.host, animated: false)
        guard scene.host.presentingViewController != nil else { return nil }
        scene.split.show(.primary)
        scene.host.view.layoutIfNeeded()
        if let glass = scene.pane, let column = glass.superview {
            let screen = scene.host.view.window?.bounds ?? UIScreen.main.bounds
            glass.frame = Snapshots.aligned(column.convert(SidebarLayout.paneRect(in: screen.size), from: nil))
        }
        return scene
    }

    func dismiss() {
        if host.presentingViewController != nil { host.dismiss(animated: false) }
    }

    func scrolls(sidebar: Bool, detail: Bool) -> [UIScrollView] {
        var scrolls: [UIScrollView] = []
        if sidebar { scrolls.append(list) }
        if detail { scrolls.append(self.detail) }
        return scrolls
    }

    func layout() -> SidebarLayout {
        let screen = host.view.window?.bounds ?? UIScreen.main.bounds
        return SidebarLayout(
            screen: screen.size, sidebar: list.superview?.convert(list.frame, to: nil) ?? .zero,
            detail: detail.superview?.convert(detail.frame, to: nil) ?? .zero, collapsed: split.isCollapsed,
            shown: split.displayMode == .oneBesideSecondary)
    }
}

/// A band the reader judges, as fractions of the UI's width and height: where it sits when the buffer runs with the
/// UI's x and when it runs against it, and its rows (symmetric about the middle row, so a turn leaves them).
struct SidebarBand {
    let x: ClosedRange<Double>
    let mirroredX: ClosedRange<Double>
    let y: ClosedRange<Double>
}

/// Where ui.glass_sidebar judges the sidebar: the trailing part of its rows (x 62% to 88% of the list's width; the
/// titles are short and the rows carry no accessory, selection or scroll indicator), over the middle 40% of the
/// screen's height. That band is symmetric about the screen's middle row, so the two ways the panel's portrait buffer
/// may turn the UI only move it along the turned axis: to `x` or to `mirroredX`.
struct SidebarLayout {
    let screen: CGSize
    let sidebar: CGRect
    let detail: CGRect
    let collapsed: Bool
    let shown: Bool

    var region: CGRect {
        CGRect(
            x: sidebar.minX + sidebar.width * 0.62, y: screen.height * 0.3, width: sidebar.width * 0.26,
            height: screen.height * 0.4)
    }

    var x: ClosedRange<Double> { Double(region.minX / screen.width)...Double(region.maxX / screen.width) }
    var mirroredX: ClosedRange<Double> { (1 - x.upperBound)...(1 - x.lowerBound) }
    var y: ClosedRange<Double> { 0.3...0.7 }
    var band: SidebarBand { SidebarBand(x: x, mirroredX: mirroredX, y: y) }

    /// The pane, in the middle of the screen both ways so that a turn of the buffer leaves it where it was, and the
    /// band judged inside it, clear of its rounded corners and rim
    static func paneRect(in screen: CGSize) -> CGRect {
        CGRect(x: screen.width * 0.38, y: screen.height * 0.3, width: screen.width * 0.24, height: screen.height * 0.4)
    }
    static let paneBand = SidebarBand(x: 0.42...0.58, mirroredX: 0.42...0.58, y: 0.36...0.64)

    /// The pane stands over the detail alone, the sidebar clear of it.
    var paneClear: Bool { sidebar.maxX <= Self.paneRect(in: screen).minX }

    /// The detail's stripes are behind the judged band.
    var under: Bool { detail.contains(region) }

    var regionText: String {
        [region.minX, region.minY, region.width, region.height].map { String(Int($0.rounded())) }
            .joined(separator: ",")
    }
}

/// Hides the status bar: its clock would commit a frame during an idle phase and restart the driver's idle timer.
final class SidebarHost: UIViewController {
    override var prefersStatusBarHidden: Bool { true }
}

/// Sixty sidebar rows with a symbol and a short title, as Settings' sidebar has.
@MainActor
final class SidebarRows: NSObject, UICollectionViewDataSource {
    static let rows: [(String, String)] = [
        ("Wi-Fi", "wifi"), ("Bluetooth", "dot.radiowaves.left.and.right"), ("Battery", "battery.100"),
        ("General", "gear"), ("Display", "sun.max.fill"), ("Sounds", "speaker.wave.3.fill"), ("Focus", "moon.fill"),
        ("Privacy", "hand.raised.fill"), ("Wallpaper", "photo.fill"), ("Camera", "camera.fill"),
        ("Siri", "mic.fill"), ("Mail", "envelope.fill"),
    ]
    static let count = 60

    private let registration = UICollectionView.CellRegistration<UICollectionViewListCell, Int> { cell, _, row in
        var content = cell.defaultContentConfiguration()
        let (title, symbol) = SidebarRows.rows[row % SidebarRows.rows.count]
        content.text = title
        content.image = UIImage(systemName: symbol)
        cell.contentConfiguration = content
    }

    func collectionView(_ collectionView: UICollectionView, numberOfItemsInSection section: Int) -> Int { Self.count }

    func collectionView(_ collectionView: UICollectionView, cellForItemAt indexPath: IndexPath) -> UICollectionViewCell
    {
        collectionView.dequeueConfiguredReusableCell(using: registration, for: indexPath, item: indexPath.item)
    }
}

/// Scrolls its scroll views a few points every display frame, back and forth over `swing` points from where each
/// started, and shows or hides `appearing` every `appearEvery` frames.
@MainActor
final class SidebarDriver: NSObject {
    static let pointsPerFrame = 3
    static let swing = 240
    static let appearEvery = 12

    let scrolls: [UIScrollView]
    let starts: [CGFloat]
    let appearing: UIView?
    private var link: CADisplayLink?
    private var tick = 0

    init(_ scrolls: [UIScrollView], appearing: UIView? = nil) {
        self.scrolls = scrolls
        self.appearing = appearing
        starts = scrolls.map(\.contentOffset.y)
        super.init()
        let link = CADisplayLink(target: self, selector: #selector(step))
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    @objc func step() {
        tick += 1
        let phase = tick * Self.pointsPerFrame % (2 * Self.swing)
        let offset = CGFloat(phase < Self.swing ? phase : 2 * Self.swing - phase)
        for (scroll, start) in zip(scrolls, starts) { scroll.contentOffset.y = start + offset }
        if let appearing, tick % Self.appearEvery == 0 { appearing.isHidden.toggle() }
    }

    func stop() {
        link?.invalidate()
        link = nil
    }
}
