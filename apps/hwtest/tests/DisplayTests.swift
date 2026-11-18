import Foundation
import HWTestCore
import QuartzCore
import UIKit

/// Records CADisplayLink timestamps while a full-screen view changes colour every frame, so every frame is a real
/// commit through the render server and the display. Measuring starts once the link runs at the 60 Hz it asked for:
/// the render server runs at the rate its clients' arbitration yields (20 Hz between tests on device) and applies a
/// raised rate only at the next frame boundary of its old cadence (24B119 IOMFBDisplay::
/// recompute_server_frame_interval_locked enqueues the new interval at a time aligned to the old one,
/// 0x1846b33ac-0x1846b34fc).
@MainActor
final class PacingRecorder: NSObject {
    private(set) var timestamps: [CFTimeInterval] = []
    /// From the link's start to its second frame at the granted 60 Hz
    private(set) var settled: CFTimeInterval = 0
    private var link: CADisplayLink?
    private let view: UIView
    private let done: Waiter<Bool>
    private let seconds: Double
    private var started: CFTimeInterval = 0
    private var until: CFTimeInterval = 0
    private var granted = 0
    private var ticks = 0
    private static let settleLimit: CFTimeInterval = 2

    init(view: UIView, seconds: Double, done: Waiter<Bool>) {
        self.view = view
        self.done = done
        self.seconds = seconds
    }

    func start() {
        started = CACurrentMediaTime()
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    @objc private func tick(_ link: CADisplayLink) {
        ticks += 1
        let hue = CGFloat(ticks % 120) / 120
        view.backgroundColor = UIColor(hue: hue, saturation: 0.6, brightness: 0.9, alpha: 1)
        if until == 0 {
            let atRate = abs(link.targetTimestamp - link.timestamp - 1.0 / 60) < 0.002
            granted = atRate ? granted + 1 : 0
            if granted < 2 && link.timestamp - started < Self.settleLimit { return }
            settled = link.timestamp - started
            until = link.timestamp + seconds
        }
        timestamps.append(link.timestamp)
        if link.timestamp >= until {
            link.invalidate()
            self.link = nil
            done.resume(true)
        }
    }
}

struct DisplayPacingTest: HWTest {
    let id = "display.pacing"
    let name = "Frame pacing over 10 s at 60 Hz"
    let subsystem = Subsystem.display

    func run(_ context: TestContext) async throws -> Outcome {
        let done = Waiter<Bool>()
        let recorder = await context.onStage(fullScreen: true) { stage -> PacingRecorder in
            let view = UIView(frame: stage.bounds)
            view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            stage.addSubview(view)
            return PacingRecorder(view: view, seconds: 10, done: done)
        }
        await MainActor.run { recorder.start() }
        _ = await done.wait(timeout: 20)
        let (stamps, settle) = await MainActor.run { (recorder.timestamps, recorder.settled) }
        let intervals = zip(stamps.dropFirst(), stamps).map { ($0 - $1) * 1000 }
        let period = 1000.0 / 60
        let dropped = Stats.droppedFrames(intervals: intervals, period: period)
        let worst = intervals.indices.max { intervals[$0] < intervals[$1] }
        let settled = stamps.firstIndex { $0 - (stamps.first ?? 0) > 0.5 } ?? stamps.count
        var metrics = Metrics()
        metrics.set("frames", stamps.count)
        metrics.set("settle_ms", settle * 1000, digits: 1)
        metrics.set("p50_ms", Stats.percentile(intervals, 0.5), digits: 2)
        metrics.set("p99_ms", Stats.percentile(intervals, 0.99), digits: 2)
        metrics.set("max_ms", intervals.max() ?? 0, digits: 2)
        metrics.set("max_at_ms", worst.map { (stamps[$0 + 1] - stamps[0]) * 1000 } ?? 0, digits: 1)
        metrics.set(
            "dropped_first_500ms",
            Stats.droppedFrames(intervals: Array(intervals.prefix(max(settled - 1, 0))), period: period))
        metrics.set("dropped", dropped)
        return .judged(
            [
                ("10 s of frames", stamps.count >= 590),
                ("p50 at 16.67 ms (±0.5)", abs(Stats.percentile(intervals, 0.5) - period) <= 0.5),
                ("no dropped frames", dropped == 0),
            ], metrics)
    }
}

/// edr_potential= and edr_current= are the screen's EDR headroom as apps read it. A J181 (iPad 9th generation, an sRGB
/// panel with no XDR) is expected to answer 1, and content that renders extended range for a headroom above 1 would then
/// not on it (RenderBox's and the posters' RGB10A8_2P_XR10 and BGR10_XR_sRGB targets, if headroom is what picks them).
struct DisplayModeTest: HWTest {
    let id = "display.mode"
    let name = "Screen mode and maximum frame rate"
    let subsystem = Subsystem.display

    func run(_ context: TestContext) async throws -> Outcome {
        let (fps, native, scale, nativeScale, gamut, edr) = await MainActor.run {
            () -> (Int, CGRect, CGFloat, CGFloat, Int, (CGFloat, CGFloat)?) in
            let screen = UIScreen.main
            var edr: (CGFloat, CGFloat)?
            if #available(iOS 16.0, *) { edr = (screen.potentialEDRHeadroom, screen.currentEDRHeadroom) }
            return (
                screen.maximumFramesPerSecond, screen.nativeBounds, screen.scale, screen.nativeScale,
                screen.traitCollection.displayGamut.rawValue, edr
            )
        }
        var metrics = Metrics()
        metrics.set("max_fps", fps)
        metrics.set("native", "\(Int(native.width))x\(Int(native.height))")
        metrics.set("scale", Double(scale), digits: 2)
        metrics.set("native_scale", Double(nativeScale), digits: 3)
        metrics.set("gamut", gamut == 1 ? "P3" : "sRGB")
        if let (potential, current) = edr {
            metrics.set("edr_potential", Double(potential), digits: 3)
            metrics.set("edr_current", Double(current), digits: 3)
        }
        return .judged(
            [("60 frames per second", fps == 60), ("a native size", native.width > 0 && native.height > 0)], metrics)
    }
}

struct BrightnessTest: HWTest {
    let id = "display.brightness"
    let name = "Brightness API"
    let subsystem = Subsystem.display

    func run(_ context: TestContext) async throws -> Outcome {
        let original = await MainActor.run { UIScreen.main.brightness }
        var readings: [CGFloat] = []
        for level in [0.3, 0.8] as [CGFloat] {
            await MainActor.run { UIScreen.main.brightness = level }
            try await Task.sleep(nanoseconds: 400_000_000)
            readings.append(await MainActor.run { UIScreen.main.brightness })
        }
        await MainActor.run { UIScreen.main.brightness = original }
        var metrics = Metrics()
        metrics.set("original", Double(original), digits: 3)
        metrics.set("read_0_3", Double(readings[0]), digits: 3)
        metrics.set("read_0_8", Double(readings[1]), digits: 3)
        return .judged(
            [("reads back what was set (±0.02)", abs(readings[0] - 0.3) <= 0.02 && abs(readings[1] - 0.8) <= 0.02)],
            metrics)
    }
}

/// sRGB swatches drawn by UIKit and snapshotted after a screen update: what the render server composited for this
/// window must keep their values.
struct ColourPipelineTest: HWTest {
    let id = "display.colour"
    let name = "Colour pipeline sanity"
    let subsystem = Subsystem.display

    static let swatches: [(String, [Int])] = [
        ("red", [255, 0, 0]), ("green", [0, 255, 0]), ("blue", [0, 0, 255]), ("grey", [128, 128, 128]),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let views = await context.onStage { stage -> [UIView] in
            Self.swatches.enumerated().map { index, swatch in
                let view = UIView(frame: CGRect(x: 20 + index * 70, y: 20, width: 60, height: 60))
                let c = swatch.1.map { CGFloat($0) / 255 }
                view.backgroundColor = UIColor(red: c[0], green: c[1], blue: c[2], alpha: 1)
                stage.addSubview(view)
                return view
            }
        }
        try await Task.sleep(nanoseconds: 300_000_000)
        let read = await MainActor.run { () -> [[Int]] in
            views.map { view in
                let format = UIGraphicsImageRendererFormat()
                format.scale = 1
                format.preferredRange = .standard
                let image = UIGraphicsImageRenderer(size: view.bounds.size, format: format).image { _ in
                    view.drawHierarchy(in: view.bounds, afterScreenUpdates: true)
                }
                return Self.centre(of: image.cgImage!)
            }
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for (swatch, got) in zip(Self.swatches, read) {
            metrics.set(swatch.0, got.map(String.init).joined(separator: ","))
            checks.append(("\(swatch.0) keeps \(swatch.1)", zip(got, swatch.1).allSatisfy { abs($0 - $1) <= 2 }))
        }
        return .judged(checks, metrics)
    }

    static func centre(of image: CGImage) -> [Int] {
        var pixel = [UInt8](repeating: 0, count: 4)
        let context = CGContext(
            data: &pixel, width: 1, height: 1, bitsPerComponent: 8, bytesPerRow: 4,
            space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
        context.draw(
            image, in: CGRect(x: -image.width / 2, y: -image.height / 2, width: image.width, height: image.height))
        return pixel.prefix(3).map(Int.init)
    }
}
