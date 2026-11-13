import HWTestCore
import Metal
import QuartzCore
import UIKit

/// presentDrawable:atTime: three periods past the display link's next frame, every fourth frame for three seconds, on a
/// full-screen CAMetalLayer: each drawable has to be on screen no earlier than half a period before its target
/// (presentedTime). early= counts those shown sooner, timed= those with a presentedTime at all; offset_* is presentedTime
/// minus the target.
@MainActor
final class PresentAtTimeRun: NSObject {
    let layer = CAMetalLayer()
    private let queue: MTLCommandQueue?
    private let done: Waiter<Bool>
    private let shown = TargetTimes()
    private var link: CADisplayLink?
    private var started: CFTimeInterval = 0
    private var frame = 0
    private(set) var submitted = 0
    nonisolated static let period = 1.0 / 60

    init(done: Waiter<Bool>) {
        let device = MTLCreateSystemDefaultDevice()
        queue = device?.makeCommandQueue()
        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        layer.framebufferOnly = true
        self.done = done
    }

    var offsets: [Double] { shown.offsets }

    func start() {
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        link.add(to: .main, forMode: .common)
        self.link = link
        started = CACurrentMediaTime()
    }

    @objc private func tick(_ link: CADisplayLink) {
        frame += 1
        if frame % 4 == 0, let queue, let drawable = layer.nextDrawable(), let commands = queue.makeCommandBuffer() {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = drawable.texture
            pass.colorAttachments[0].loadAction = .clear
            let level = Double(frame % 60) / 60
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 1 - level, green: level, blue: 0.4, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
            let target = link.targetTimestamp + 3 * Self.period
            let shown = shown
            drawable.addPresentedHandler { shown.add($0.presentedTime, target: target) }
            commands.present(drawable, atTime: target)
            commands.commit()
            submitted += 1
        }
        if link.timestamp - started >= 3 {
            link.invalidate()
            self.link = nil
            done.resume(true)
        }
    }
}

/// presentedTime minus target in milliseconds, for the drawables shown with a time; written on whatever thread Metal
/// reports them.
final class TargetTimes: @unchecked Sendable {
    private let lock = NSLock()
    private var values: [Double] = []

    func add(_ presented: Double, target: Double) {
        guard presented > 0 else { return }
        lock.lock()
        defer { lock.unlock() }
        values.append((presented - target) * 1000)
    }

    var offsets: [Double] {
        lock.lock()
        defer { lock.unlock() }
        return values
    }
}

struct PresentAtTimeTest: HWTest {
    let id = "display.present_at_time"
    let name = "presentDrawable:atTime: shows no drawable before its time"
    let subsystem = Subsystem.compositor

    func run(_ context: TestContext) async throws -> Outcome {
        let done = Waiter<Bool>()
        let run = await context.onStage(fullScreen: true) { stage -> PresentAtTimeRun in
            let run = PresentAtTimeRun(done: done)
            run.layer.frame = stage.bounds
            run.layer.contentsScale = UIScreen.main.scale
            stage.layer.addSublayer(run.layer)
            return run
        }
        await MainActor.run { run.start() }
        _ = await done.wait(timeout: 10)
        try await Task.sleep(nanoseconds: 300_000_000)
        let (submitted, offsets) = await MainActor.run { (run.submitted, run.offsets) }
        let half = PresentAtTimeRun.period * 1000 / 2
        let early = offsets.filter { $0 < -half }

        var metrics = Metrics()
        metrics.set("submitted", submitted)
        metrics.set("timed", offsets.count)
        metrics.set("early", early.count)
        metrics.set("offset_min_ms", offsets.min() ?? 0, digits: 2)
        metrics.set("offset_p50_ms", Stats.percentile(offsets, 0.5), digits: 2)
        metrics.set("offset_max_ms", offsets.max() ?? 0, digits: 2)
        return .judged(
            [
                ("drawables presented with a time", submitted > 20 && offsets.count >= submitted / 2),
                ("none shown more than half a period before its time (early 0)", early.isEmpty),
            ], metrics)
    }
}
