import HWTestKit
import QuartzCore
import UIKit

/// The main screen's list under its large title (kit/ContentView.swift: "Hardware tests"), scrolled by program: down by
/// `depth` and back over `seconds` each way at every display-link frame, so the title collapses into the bar, its
/// progressive blur and text fade appear, and the title opens again.
@MainActor
final class TitleScroller: NSObject {
    struct Cycle {
        let start: Double
        let end: Double
        /// The navigation bar's least and greatest height over the cycle: the large title collapsing moves them apart.
        let barMin: CGFloat
        let barMax: CGFloat
        let deepest: CGFloat
        let frames: Int
    }

    let scroll: UIScrollView
    let bar: UINavigationBar?
    let window: UIWindow
    private let rest: CGPoint
    private var link: CADisplayLink?
    private var cycle: (start: Double, seconds: Double, depth: CGFloat, done: Waiter<Cycle>)?
    private var barMin = CGFloat.greatestFiniteMagnitude, barMax: CGFloat = 0, deepest: CGFloat = 0, frames = 0

    /// The key window's tallest scroll view and its navigation bar; nil when the window holds no scroll view.
    static func find() -> TitleScroller? {
        guard let window = UIApplication.shared.connectedScenes.compactMap({ ($0 as? UIWindowScene)?.keyWindow }).first
        else { return nil }
        let views = all(in: window)
        guard let scroll = views.compactMap({ $0 as? UIScrollView }).max(by: { $0.bounds.height < $1.bounds.height })
        else { return nil }
        return TitleScroller(scroll: scroll, bar: views.compactMap { $0 as? UINavigationBar }.first, window: window)
    }

    private static func all(in view: UIView) -> [UIView] {
        [view] + view.subviews.flatMap { all(in: $0) }
    }

    private init(scroll: UIScrollView, bar: UINavigationBar?, window: UIWindow) {
        self.scroll = scroll
        self.bar = bar
        self.window = window
        rest = scroll.contentOffset
    }

    /// One cycle down and back, its start and end on CACurrentMediaTime's clock.
    func run(depth: CGFloat, seconds: Double) async -> Cycle {
        let done = Waiter<Cycle>()
        barMin = .greatestFiniteMagnitude
        barMax = 0
        deepest = 0
        frames = 0
        cycle = (CACurrentMediaTime(), seconds, depth, done)
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        link.add(to: .main, forMode: .common)
        self.link = link
        let started = CACurrentMediaTime()
        return await done.wait(timeout: 2 * seconds + 3)
            ?? Cycle(start: started, end: CACurrentMediaTime(), barMin: 0, barMax: 0, deepest: 0, frames: 0)
    }

    /// The list back where the test found it.
    func restore() {
        link?.invalidate()
        link = nil
        scroll.setContentOffset(rest, animated: false)
    }

    @objc private func tick(_ link: CADisplayLink) {
        guard let cycle else { return }
        let elapsed = link.targetTimestamp - cycle.start
        let half = cycle.seconds
        let t = min(max(elapsed / half, 0), 2)
        let along = t <= 1 ? t : 2 - t
        let eased = CGFloat(along * along * (3 - 2 * along))
        scroll.contentOffset = CGPoint(x: rest.x, y: rest.y + eased * cycle.depth)
        frames += 1
        deepest = max(deepest, scroll.contentOffset.y - rest.y)
        if let height = bar?.bounds.height {
            barMin = min(barMin, height)
            barMax = max(barMax, height)
        }
        guard t >= 2 else { return }
        link.invalidate()
        self.link = nil
        self.cycle = nil
        cycle.done.resume(
            Cycle(
                start: cycle.start, end: CACurrentMediaTime(), barMin: barMin == .greatestFiniteMagnitude ? 0 : barMin,
                barMax: barMax, deepest: deepest, frames: frames
            )
        )
    }
}
