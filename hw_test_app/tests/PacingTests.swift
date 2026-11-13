import Foundation
import HWTestCore
import Metal
import QuartzCore
import UIKit

/// Frame pacing per scene: a CADisplayLink at 60 Hz animates the scene every frame for five seconds after a half
/// second's warm-up, while a probe CAMetalLayer presents a cleared drawable each frame and collects when the display
/// actually showed it (presentedTime). link_* fields are the display link's intervals (frames the app's main thread
/// missed), present_* the probe's (frames the compositor and display missed), late= the callbacks that began after
/// their frame's target time; hist= counts intervals of 1, 2, 3 and 4+ periods.
@MainActor
final class PacingRun: NSObject {
    let probe = CAMetalLayer()
    private let queue: MTLCommandQueue?
    private let animate: (Int) -> Void
    private let done: Waiter<Bool>
    private let shown = ShownTimes()
    private var link: CADisplayLink?
    private var started: CFTimeInterval = 0
    private var frame = 0
    private(set) var linkTimes: [CFTimeInterval] = []
    private(set) var late = 0
    private(set) var nilDrawables = 0
    /// The first drawable's texture and its IOSurface, as "WxH/WxH": a quarter-turned drawable on the portrait panel
    /// shows as 1280x720/720x1280.
    private(set) var drawableShape = "-"
    let seconds: Double

    init(seconds: Double, animate: @escaping (Int) -> Void, done: Waiter<Bool>) {
        let device = MTLCreateSystemDefaultDevice()
        queue = device?.makeCommandQueue()
        probe.device = device
        probe.pixelFormat = .bgra8Unorm
        probe.framebufferOnly = true
        self.seconds = seconds
        self.animate = animate
        self.done = done
    }

    var presented: [Double] { shown.measured }

    /// Commit to on glass of each measured probe frame, in milliseconds (presentedTime is CACurrentMediaTime's clock).
    var presentLatencies: [Double] { shown.latencies }

    /// When the display showed the probe's first drawable (0 until it has).
    var firstPresented: Double { shown.first }

    func start() {
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
        commands.commit()
    }
}

/// Presented times, written on whatever thread Metal reports them.
final class ShownTimes: @unchecked Sendable {
    private let lock = NSLock()
    private var times: [Double] = []
    private var delays: [Double] = []
    private var firstTime: Double = 0

    func add(_ time: Double, submitted: Double, measured: Bool) {
        lock.lock()
        defer { lock.unlock() }
        if measured { times.append(time) }
        if measured && time > 0 { delays.append((time - submitted) * 1000) }
        if firstTime == 0 && time > 0 { firstTime = time }
    }

    var latencies: [Double] {
        lock.lock()
        defer { lock.unlock() }
        return delays
    }

    var measured: [Double] {
        lock.lock()
        defer { lock.unlock() }
        return times
    }

    var first: Double {
        lock.lock()
        defer { lock.unlock() }
        return firstTime
    }
}

public struct PacingTest: HWTest {
    public let id: String
    public let name: String
    public let subsystem = Subsystem.compositor
    let probeSize: CGFloat?
    let build: @MainActor @Sendable (UIView, CGRect) -> (Int) -> Void

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
            ], metrics)
    }
}

enum PacingTests {
    static let all: [any HWTest] = [metal, text, blurOver, blurUnder, list]

    static let metal = PacingTest(
        id: "pacing.metal_layer", name: "Pacing: a full-screen Metal layer alone", probeSize: nil
    ) { _, _ in { _ in } }

    static let text = PacingTest(id: "pacing.text", name: "Pacing: labels redrawn every frame", probeSize: 48) {
        stage, screen in
        let labels = UITextTest.styles.enumerated().map { line, style -> UILabel in
            let label = UILabel(frame: CGRect(x: 10, y: 10 + CGFloat(line) * 38, width: screen.width - 80, height: 36))
            label.font = UIFont.preferredFont(forTextStyle: style)
            stage.addSubview(label)
            return label
        }
        stage.backgroundColor = .white
        return { frame in for (line, label) in labels.enumerated() { label.text = "Wi-Fi \(frame * (line + 1))" } }
    }

    /// r45-render's (d): a full-screen layer animated over a blurred (still) backdrop.
    static let blurOver = PacingTest(
        id: "pacing.blur_over", name: "Pacing: a full-screen layer animated over a blur", probeSize: 48
    ) { stage, screen in
        stage.addSubview(BackdropStripes(frame: screen))
        let blur = UIVisualEffectView(effect: UIBlurEffect(style: .regular))
        blur.frame = screen
        stage.addSubview(blur)
        let veil = UIView(frame: screen)
        stage.addSubview(veil)
        return { frame in
            veil.backgroundColor = UIColor(
                hue: CGFloat(frame % 120) / 120, saturation: 0.7, brightness: 0.9, alpha: 0.35)
            veil.transform = CGAffineTransform(translationX: CGFloat(frame % 40) - 20, y: 0)
        }
    }

    static let blurUnder = PacingTest(
        id: "pacing.blur_under", name: "Pacing: a backdrop moving under a full-screen blur", probeSize: 48
    ) { stage, screen in
        let stripes = BackdropStripes(frame: screen.insetBy(dx: -64, dy: 0))
        stage.addSubview(stripes)
        let blur = UIVisualEffectView(effect: UIBlurEffect(style: .regular))
        blur.frame = screen
        stage.addSubview(blur)
        return { frame in stripes.transform = CGAffineTransform(translationX: CGFloat(frame % 32) * 2 - 32, y: 0) }
    }

    static let list = PacingTest(id: "pacing.list", name: "Pacing: a list scrolled every frame", probeSize: 48) {
        stage, screen in
        let table = SettingsList.make(screen)
        stage.addSubview(table)
        return { frame in table.contentOffset = CGPoint(x: 0, y: CGFloat(frame % 300) * 4) }
    }
}
