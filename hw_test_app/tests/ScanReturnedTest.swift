import Foundation
import HWTestCore
import UIKit

/// Window A never scans a page Core Animation has back, and window B never has a surface in reach whose use count it
/// does not hold (drivers/NativeMobileDisplay/SwitchScanReturnDisplay.h, debug kernels): a page goes back when the swap
/// after the one that showed it retires, so it must be out of window A's reach then (not scanned, asked for, replaced,
/// held or armed for a completion flip), and the page window A scans at each frame end must not be one Core Animation
/// has back. Runs display.pacing's full-screen colour change, then stripes scrolled under a blur every frame (heavier
/// frames, two swaps inside one refresh), and reads kern.switch_scan_{checks,handed,overlaps} around them; the boot's
/// total of overlaps covers everything shown since boot. A failed check is a buffer written while the panel shows it:
/// black or torn bands along the panel's scanlines.
struct ScanReturnedTest: HWTest {
    let id = "display.scan_returned"
    let name = "No buffer goes back to its owner while the display can still scan it"
    let subsystem = Subsystem.display

    static let counters = ["checks", "handed", "overlaps"]

    func run(_ context: TestContext) async throws -> Outcome {
        let (before, unread) = Self.totals()
        guard !before.isEmpty else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(unread))
            return .skip(KernelSysctl.unreadable("kern.switch_scan_*", unread), metrics)
        }
        let done = Waiter<Bool>()
        let recorder = await context.onStage(fullScreen: true) { stage -> PacingRecorder in
            let view = UIView(frame: stage.bounds)
            view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            stage.addSubview(view)
            return PacingRecorder(view: view, seconds: 5, done: done)
        }
        await MainActor.run { recorder.start() }
        _ = await done.wait(timeout: 15)
        let frames = await MainActor.run { recorder.timestamps.count }
        await context.clearStage()
        let scroller = await context.onStage(fullScreen: true) { stage in BlurScroller(stage) }
        await MainActor.run { scroller.start() }
        try? await Task.sleep(nanoseconds: 4_000_000_000)
        await MainActor.run { scroller.stop() }
        await context.clearStage()
        let after = Self.totals().values
        let delta = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }

        var metrics = Metrics()
        for name in Self.counters { metrics.set("d_" + name, delta(name)) }
        metrics.set("overlaps_boot", after["overlaps"] ?? 0)
        metrics.set("frames", frames)
        metrics.set("scrolled", await MainActor.run { scroller.ticks })
        return .judged(
            [
                ("the display checked its frames (d_checks >= 300)", delta("checks") >= 300),
                ("pages went back to Core Animation (d_handed >= 100)", delta("handed") >= 100),
                ("none while the display could scan it (d_overlaps = 0)", delta("overlaps") == 0),
                ("none since boot (overlaps_boot = 0)", (after["overlaps"] ?? 0) == 0),
            ], metrics)
    }

    /// kern.switch_scan_* by name, and the errno of the first that could not be read.
    static func totals() -> (values: [String: Int], unread: Int32) {
        var values: [String: Int] = [:]
        var unread: Int32 = 0
        for name in counters {
            switch KernelSysctl.integer("kern.switch_scan_" + name, as: Int32.self) {
            case .value(let value): values[name] = Int(value)
            case .failed(let error): if unread == 0 { unread = error }
            }
        }
        return (values, unread)
    }
}

/// White and grey stripes scrolled every display frame under a blurred band across the screen's middle.
@MainActor
final class BlurScroller: NSObject {
    let scroll: UIScrollView
    private var link: CADisplayLink?
    private(set) var ticks = 0

    init(_ stage: UIView) {
        let bounds = stage.bounds
        scroll = UIScrollView(frame: bounds)
        let stripes = UIView(frame: CGRect(x: 0, y: 0, width: bounds.width * 3, height: bounds.height))
        stripes.backgroundColor = UIColor(white: 0.45, alpha: 1)
        for x in stride(from: CGFloat(0), to: stripes.bounds.width, by: 48) {
            let stripe = UIView(frame: CGRect(x: x, y: 0, width: 24, height: bounds.height))
            stripe.backgroundColor = .white
            stripes.addSubview(stripe)
        }
        scroll.addSubview(stripes)
        scroll.contentSize = stripes.bounds.size
        stage.addSubview(scroll)
        let blur = UIVisualEffectView(effect: UIBlurEffect(style: .regular))
        blur.frame = CGRect(x: 0, y: bounds.height * 0.3, width: bounds.width, height: bounds.height * 0.4)
        stage.addSubview(blur)
    }

    func start() {
        let link = CADisplayLink(target: self, selector: #selector(step))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    @objc private func step() {
        ticks += 1
        let width = scroll.bounds.width
        scroll.contentOffset.x = CGFloat(ticks * 4).truncatingRemainder(dividingBy: width * 2)
    }

    func stop() {
        link?.invalidate()
        link = nil
    }
}
