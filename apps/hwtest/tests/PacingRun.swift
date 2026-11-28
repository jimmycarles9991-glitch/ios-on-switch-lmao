import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal
import QuartzCore
import UIKit

/// Frame pacing per scene: a CADisplayLink at 60 Hz animates the scene every frame for five seconds after a half
/// second's warm-up, while a probe CAMetalLayer presents a cleared drawable each frame and collects when the display
/// actually showed it (presentedTime). link_* fields are the display link's intervals (frames the app's main thread
/// missed), present_* the probe's (frames the compositor and display missed), late= the callbacks that began after
/// their frame's target time; hist= counts intervals of 1, 2, 3 and 4+ periods.
@MainActor
public final class PacingRun: NSObject {
    public let probe = CAMetalLayer()
    private let queue: MTLCommandQueue?
    private let animate: (Int) -> Void
    private let done: Waiter<Bool>
    private let shown = ShownTimes()
    private var link: CADisplayLink?
    private var started: CFTimeInterval = 0
    private var frame = 0
    public private(set) var linkTimes: [CFTimeInterval] = []
    public private(set) var late = 0
    public private(set) var nilDrawables = 0
    public nonisolated let paths = CommandPaths()
    /// The first drawable's texture and its IOSurface, as "WxH/WxH": a quarter-turned drawable on the portrait panel
    /// shows as 1280x720/720x1280.
    public private(set) var drawableShape = "-"
    public let seconds: Double

    public init(seconds: Double, animate: @escaping (Int) -> Void, done: Waiter<Bool>) {
        let device = MTLCreateSystemDefaultDevice()
        queue = device?.makeCommandQueue()
        probe.device = device
        probe.pixelFormat = .bgra8Unorm
        probe.framebufferOnly = true
        self.seconds = seconds
        self.animate = animate
        self.done = done
    }

    public var presented: [Double] { shown.measured }

    /// Commit to on glass of each measured probe frame, in milliseconds (presentedTime is CACurrentMediaTime's clock).
    public var presentLatencies: [Double] { shown.latencies }

    /// When the display showed the probe's first drawable (0 until it has).
    public var firstPresented: Double { shown.first }

    public func start() {
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        link.add(to: .main, forMode: .common)
        self.link = link
        started = CACurrentMediaTime()
    }

    @objc private func tick(_ link: CADisplayLink) {
        frame += 1
        let measuring = link.timestamp - started > 0.5
        if measuring {
            linkTimes.append(link.timestamp)
            if CACurrentMediaTime() > link.targetTimestamp { late += 1 }
        }
        animate(frame)
        present(measuring)
        if link.timestamp - started >= seconds + 0.5 {
            link.invalidate()
            self.link = nil
            done.resume(true)
        }
    }

    private func present(_ measuring: Bool) {
        guard let queue else { return }
        guard let drawable = probe.nextDrawable() else {
            if measuring { nilDrawables += 1 }
            return
        }
        if drawableShape == "-" {
            let texture = drawable.texture
            let surface = texture.iosurface.map { "\(IOSurfaceGetWidth($0))x\(IOSurfaceGetHeight($0))" } ?? "none"
            drawableShape = "\(texture.width)x\(texture.height)/\(surface)"
        }
        guard let commands = queue.makeCommandBuffer() else { return }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = drawable.texture
        pass.colorAttachments[0].loadAction = .clear
        let level = Double(frame % 60) / 60
        pass.colorAttachments[0].clearColor = MTLClearColor(red: level, green: 0.3, blue: 1 - level, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        commands.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
        let shown = shown
        let submitted = CACurrentMediaTime()
        drawable.addPresentedHandler { shown.add($0.presentedTime, submitted: submitted, measured: measuring) }
        commands.present(drawable)
        paths.watch(commands)
        commands.commit()
    }
}

/// Presented times, written on whatever thread Metal reports them.
public final class ShownTimes: @unchecked Sendable {
    private let lock = NSLock()
    private var times: [Double] = []
    private var delays: [Double] = []
    private var firstTime: Double = 0

    public init() {}

    public func add(_ time: Double, submitted: Double, measured: Bool) {
        lock.lock()
        defer { lock.unlock() }
        if measured { times.append(time) }
        if measured && time > 0 { delays.append((time - submitted) * 1000) }
        if firstTime == 0 && time > 0 { firstTime = time }
    }

    public var latencies: [Double] {
        lock.lock()
        defer { lock.unlock() }
        return delays
    }

    public var measured: [Double] {
        lock.lock()
        defer { lock.unlock() }
        return times
    }

    public var first: Double {
        lock.lock()
        defer { lock.unlock() }
        return firstTime
    }
}

public struct PacingTest: HWTest {
    public let id: String
    public let name: String
    public let subsystem = Subsystem.compositor
    public let probeSize: CGFloat?
    public let build: @MainActor @Sendable (UIView, CGRect) -> (Int) -> Void

    public init(
        id: String, name: String, probeSize: CGFloat?,
        build: @escaping @MainActor @Sendable (UIView, CGRect) -> (Int) -> Void
    ) {
        self.id = id
        self.name = name
        self.probeSize = probeSize
        self.build = build
    }

    public var timeout: TimeInterval { 20 }

    public func run(_ context: TestContext) async throws -> Outcome {
        let done = Waiter<Bool>()
        let recorder = await context.onStage(fullScreen: true) { stage -> PacingRun in
            let screen = UIScreen.main.bounds
            let animate = build(stage, screen)
            let recorder = PacingRun(seconds: 5, animate: animate, done: done)
            let side = probeSize ?? 0
            recorder.probe.frame =
                side > 0 ? CGRect(x: screen.width - side - 10, y: 10, width: side, height: side) : screen
            recorder.probe.contentsScale = UIScreen.main.scale
            stage.layer.addSublayer(recorder.probe)
            return recorder
        }
        await MainActor.run { recorder.start() }
        _ = await done.wait(timeout: 15)
        try await Task.sleep(nanoseconds: 200_000_000)
        let (linkTimes, presented, late, latencies, nilDrawables, drawableShape) = await MainActor.run {
            (
                recorder.linkTimes, recorder.presented, recorder.late, recorder.presentLatencies,
                recorder.nilDrawables, recorder.drawableShape
            )
        }
        let link = Pacing(times: linkTimes, period: 1.0 / 60)
        let shown = Pacing(times: presented.sorted(), period: 1.0 / 60)
        var metrics = Metrics()
        metrics.merge(link.metrics(prefix: "link_"))
        metrics.set("late", late)
        metrics.merge(shown.metrics(prefix: "present_"))
        metrics.set("unshown", presented.filter { $0 <= 0 }.count)
        metrics.set("present_latency_p50_ms", Stats.percentile(latencies, 0.5), digits: 2)
        metrics.set("present_latency_p95_ms", Stats.percentile(latencies, 0.95), digits: 2)
        metrics.set("nil_drawables", nilDrawables)
        metrics.set("drawable", drawableShape)
        return .judged(
            [
                ("nextDrawable returned nil \(nilDrawables) times (nil_drawables)", nilDrawables == 0),
                ("\(link.frames) display-link frames in 5 s, under 290", link.frames >= 290),
                ("the app missed \(link.missed) frames (link_missed)", link.missed == 0),
                ("\(shown.frames) of \(link.frames) probe frames shown with a time", shown.frames >= link.frames / 2),
                ("the display missed \(shown.missed) frames (present_missed)", shown.missed == 0),
            ], metrics
        ).onRawPath(recorder.paths, device: MTLCreateSystemDefaultDevice())
    }
}
