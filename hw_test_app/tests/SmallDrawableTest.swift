import HWTestCore
import Metal
import QuartzCore
import UIKit

/// A full-screen CAMetalLayer whose drawableSize is half its pixel size, presented every frame for two seconds: the
/// display publishes RGB scaling from 1/4 to 4x (IOMFBScalingLimits) and programs window B's DDA filters for a scaled
/// layer, so Core Animation can detach the layer onto window B and the panel shows it scaled with no GPU pass. Reads
/// the kernel's plane counters around it (kern.switch_planes_*, debug kernels): window B has to have flipped and
/// latched, and nothing it was given may have gone unshown.
@MainActor
final class SmallDrawableRun: NSObject {
    let layer = CAMetalLayer()
    private let queue: MTLCommandQueue?
    private let done: Waiter<Bool>
    private var link: CADisplayLink?
    private var started: CFTimeInterval = 0
    private(set) var presented = 0

    init(done: Waiter<Bool>) {
        let device = MTLCreateSystemDefaultDevice()
        queue = device?.makeCommandQueue()
        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        layer.framebufferOnly = true
        layer.isOpaque = true
        self.done = done
    }

    func start() {
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        link.add(to: .main, forMode: .common)
        self.link = link
        started = CACurrentMediaTime()
    }

    @objc private func tick(_ link: CADisplayLink) {
        if let queue, let drawable = layer.nextDrawable(), let commands = queue.makeCommandBuffer() {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = drawable.texture
            pass.colorAttachments[0].loadAction = .clear
            let level = Double(presented % 60) / 60
            pass.colorAttachments[0].clearColor = MTLClearColor(red: level, green: 0.5, blue: 1 - level, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
            commands.present(drawable)
            commands.commit()
            presented += 1
        }
        if link.timestamp - started >= 2 {
            link.invalidate()
            self.link = nil
            done.resume(true)
        }
    }
}

struct SmallDrawableTest: HWTest {
    let id = "display.small_drawable"
    let name = "A half-size Metal drawable scaled by the display's window, not the GPU"
    let subsystem = Subsystem.compositor

    func run(_ context: TestContext) async throws -> Outcome {
        let (before, unread) = PlanesTest.totals()
        guard !before.isEmpty else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(unread))
            return .skip(KernelSysctl.unreadable("kern.switch_planes_*", unread), metrics)
        }
        let done = Waiter<Bool>()
        let (run, pixels, drawable) = await context.onStage(fullScreen: true) {
            stage -> (SmallDrawableRun, CGSize, CGSize) in
            let run = SmallDrawableRun(done: done)
            let scale = UIScreen.main.scale
            run.layer.frame = stage.bounds
            run.layer.contentsScale = scale
            let pixels = CGSize(width: stage.bounds.width * scale, height: stage.bounds.height * scale)
            let drawable = CGSize(width: (pixels.width / 2).rounded(), height: (pixels.height / 2).rounded())
            run.layer.drawableSize = drawable
            stage.layer.addSublayer(run.layer)
            return (run, pixels, drawable)
        }
        await MainActor.run { run.start() }
        _ = await done.wait(timeout: 10)
        try await Task.sleep(nanoseconds: 200_000_000)
        let after = PlanesTest.totals().values
        let delta = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }
        let presented = await MainActor.run { run.presented }

        var metrics = Metrics()
        metrics.set("layer", "\(Int(pixels.width))x\(Int(pixels.height))")
        metrics.set("drawable", "\(Int(drawable.width))x\(Int(drawable.height))")
        metrics.set("presented", presented)
        for name in PlanesTest.counters { metrics.set("d_" + name, delta(name)) }
        return .judged(
            [
                ("the app presented frames", presented > 60),
                ("window B flipped (d_flips > 0)", delta("flips") > 0),
                ("window B took its sets (d_latched > 0)", delta("latched") > 0),
                ("nothing window B was given went unshown (d_dropped 0)", delta("dropped") == 0),
            ], metrics)
    }
}
