import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import UIKit

/// What a screenshot takes and shows. The render server's own render of the presented pattern (drawHierarchy, the
/// render-to-surface path a screenshot's capture takes) has to match the CPU's drawing of it, and ScreenshotServices'
/// flash over the pattern, sampled the same way while it runs, has to brighten the screen and never black it out (styles
/// 0 and 2; style 1, a plus-darker flash, is only measured).
struct DisplaySnapshotTest: HWTest {
    let id = "display.snapshot"
    let name = "Display snapshot and the screenshot flash"
    let subsystem = Subsystem.compositor

    static let flashStyles: [UInt] = [0, 1, 2]
    static let sampleDelays: [Double] = [0.1, 0.3, 0.6, 1.0]
    static let nearBlack: Float = 0.06

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        var reasons: [String] = []
        let (pattern, check) = await UISceneTests.staged(context) { stage, screen -> UIView in
            let root = SnapshotPattern(frame: Snapshots.aligned(screen))
            stage.addSubview(root)
            return root
        } _: { root -> (UIView, CompositeCheck) in
            (root, CompositeCheck(label: "pattern", composited: Snapshots.composited(root), cpu: Snapshots.cpu(root)))
        }
        check.judge(&metrics, &reasons, "pattern")
        let patternMean = Self.mean(check.composited)
        metrics.set("pattern_mean", Self.level(patternMean))
        metrics.set("pattern_black_pct", Self.blackPercent(check.composited))
        if Self.blackPercent(check.composited) > 0 { reasons.append("the rendered pattern has black pixels") }

        for style in Self.flashStyles {
            guard let samples = await Self.flash(style: style, over: pattern) else {
                metrics.set("flash_absent", 1)
                break
            }
            let means = samples.map(Self.mean)
            let blacks = samples.map(Self.blackPercent)
            metrics.set("flash\(style)_min_mean", Self.level(means.min() ?? 0))
            metrics.set("flash\(style)_max_black_pct", blacks.max() ?? 0)
            guard style != 1 else { continue }
            if let worst = blacks.max(), worst > 25 {
                reasons.append("flash style \(style): \(worst)% of a frame black")
            }
            if let darkest = means.min(), darkest < patternMean - 0.02 {
                reasons.append(
                    "flash style \(style) darkened the screen (\(Self.level(darkest)) < \(Self.level(patternMean)))")
            }
        }
        if !reasons.isEmpty { check.save(to: context.documents, id: id, stem: "pattern") }
        return reasons.isEmpty ? .pass(metrics) : .fail(LogLine.fitted(reasons), metrics)
    }

    /// The render server's renders of `pattern` with ScreenshotServices' flash of `style` running over it, at
    /// `sampleDelays` from its start; nil without the flash class.
    static func flash(style: UInt, over pattern: UIView) async -> [Raster]? {
        guard let view = await MainActor.run(body: { flashView(style: style, frame: pattern.bounds) }) else {
            return nil
        }
        await MainActor.run {
            pattern.addSubview(view)
            let done: @convention(block) () -> Void = {}
            _ = view.perform(
                NSSelectorFromString("flashWithCompletion:"), with: unsafeBitCast(done, to: AnyObject.self))
        }
        var samples: [Raster] = []
        var elapsed = 0.0
        for delay in sampleDelays {
            try? await Task.sleep(nanoseconds: UInt64((delay - elapsed) * 1_000_000_000))
            elapsed = delay
            samples.append(await MainActor.run { Snapshots.composited(pattern) })
        }
        try? await Task.sleep(nanoseconds: 2_000_000_000)
        await MainActor.run { view.removeFromSuperview() }
        return samples
    }

    @MainActor
    static func flashView(style: UInt, frame: CGRect) -> UIView? {
        let path = "/System/Library/PrivateFrameworks/ScreenshotServices.framework/ScreenshotServices"
        let selector = NSSelectorFromString("flashViewForStyle:")
        guard dlopen(path, RTLD_NOW) != nil, let flashClass = NSClassFromString("SSFlashView") as? NSObject.Type,
            flashClass.responds(to: selector)
        else { return nil }
        typealias Make = @convention(c) (AnyClass, Selector, UInt) -> Unmanaged<UIView>?
        let make = unsafeBitCast(flashClass.method(for: selector), to: Make.self)
        guard let view = make(flashClass, selector, style)?.takeUnretainedValue() else { return nil }
        view.frame = frame
        return view
    }

    static func mean(_ raster: Raster) -> Float {
        raster.samples.reduce(0, +) / Float(max(raster.samples.count, 1))
    }

    static func blackPercent(_ raster: Raster) -> Int {
        var black = 0
        for y in 0..<raster.height {
            for x in 0..<raster.width where raster.pixel(x, y).allSatisfy({ $0 < nearBlack }) { black += 1 }
        }
        return black * 100 / max(raster.width * raster.height, 1)
    }

    static func level(_ value: Float) -> Int { Int((value * 255).rounded()) }
}

/// Red, green, blue and white quadrants with a mid-grey band across them: no black anywhere, each channel half lit.
final class SnapshotPattern: UIView {
    override init(frame: CGRect) {
        super.init(frame: frame)
        let (w, h) = (frame.width / 2, frame.height / 2)
        let quadrants: [(CGRect, UIColor)] = [
            (CGRect(x: 0, y: 0, width: w, height: h), .red), (CGRect(x: w, y: 0, width: w, height: h), .green),
            (CGRect(x: 0, y: h, width: w, height: h), .blue), (CGRect(x: w, y: h, width: w, height: h), .white),
        ]
        for (rect, colour) in quadrants {
            let quadrant = UIView(frame: Snapshots.aligned(rect))
            quadrant.backgroundColor = colour
            addSubview(quadrant)
        }
        let band = UIView(frame: Snapshots.aligned(CGRect(x: 0, y: h - 40, width: frame.width, height: 80)))
        band.backgroundColor = UIColor(white: 0.5, alpha: 1)
        addSubview(band)
    }

    required init?(coder: NSCoder) { nil }
}
