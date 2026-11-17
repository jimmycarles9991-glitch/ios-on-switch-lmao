import HWTestCore
import HWTestGPU
import Metal
import QuartzCore
import UIKit

/// How a phase of display.present_at_time presents its drawables, every fourth display-link frame.
enum PresentMode: Sendable {
    /// presentDrawable:atTime: three periods past the link's next frame (the case judged since boot 19).
    case linkTarget
    /// presentDrawable: with no time.
    case plain
    /// presentDrawable:atTime: a tenth of a second before the commit: due at once.
    case past
    /// presentDrawable:atTime: four periods after CACurrentMediaTime(), the display link not involved.
    case mediaTarget
    /// Plain presents for half a second, then linkTarget's on the same layer: a layer Core Animation has already shown.
    case warm
    /// CAMetalDisplayLink owns the drawable's target time; the command buffer presents it without an explicit time.
    case metalLink
}

/// One phase on a fresh CAMetalLayer: what each drawable asked for and what Metal told it back.
@MainActor
final class PresentAtTimeRun: NSObject, CAMetalDisplayLinkDelegate {
    let layer = CAMetalLayer()
    private let queue: MTLCommandQueue?
    private let mode: PresentMode
    private let seconds: Double
    private let done: Waiter<Bool>
    nonisolated let paths: CommandPaths
    private let frames = PresentedFrames()
    private var link: CADisplayLink?
    private var metalLink: CAMetalDisplayLink?
    private var started: CFTimeInterval = 0
    private var tick = 0
    private(set) var drawableSize = CGSize.zero

    init(mode: PresentMode, seconds: Double, done: Waiter<Bool>, paths: CommandPaths) {
        let device = MTLCreateSystemDefaultDevice()
        queue = device?.makeCommandQueue()
        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        layer.framebufferOnly = true
        self.mode = mode
        self.seconds = seconds
        self.done = done
        self.paths = paths
    }

    var results: [PresentTimeVerdict.Frame] { frames.all }

    func start() {
        let rate = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        started = CACurrentMediaTime()
        if mode == .metalLink {
            let link = CAMetalDisplayLink(metalLayer: layer)
            link.delegate = self
            link.preferredFrameRateRange = rate
            link.add(to: .main, forMode: .common)
            metalLink = link
            return
        }
        let link = CADisplayLink(target: self, selector: #selector(step))
        link.preferredFrameRateRange = rate
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    @objc private func step(_ link: CADisplayLink) {
        tick += 1
        let warming = mode == .warm && link.timestamp - started < 0.5
        if tick % 4 == 0, let drawable = layer.nextDrawable() {
            let committed = CACurrentMediaTime()
            let period = PresentTimeVerdict.period
            let target: Double
            switch mode {
            case .linkTarget, .warm, .metalLink: target = warming ? 0 : link.targetTimestamp + 3 * period
            case .plain: target = 0
            case .past: target = committed - 0.1
            case .mediaTarget: target = committed + 4 * period
            }
            present(drawable, target: target, recorded: !warming)
        }
        if link.timestamp - started >= seconds {
            link.invalidate()
            self.link = nil
            done.resume(true)
        }
    }

    nonisolated func metalDisplayLink(_ link: CAMetalDisplayLink, needsUpdate update: CAMetalDisplayLink.Update) {
        MainActor.assumeIsolated {
            tick += 1
            if tick % 4 == 0 {
                present(update.drawable, target: update.targetPresentationTimestamp)
            }
            if CACurrentMediaTime() - started >= seconds {
                link.invalidate()
                metalLink = nil
                done.resume(true)
            }
        }
    }

    /// Clears the drawable and presents it at `target` (0: plainly), recording what Metal reports for it.
    private func present(_ drawable: CAMetalDrawable, target: Double, recorded: Bool = true) {
        guard let queue, let commands = queue.makeCommandBuffer() else { return }
        drawableSize = CGSize(width: drawable.texture.width, height: drawable.texture.height)
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = drawable.texture
        pass.colorAttachments[0].loadAction = .clear
        let level = Double(tick % 60) / 60
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 1 - level, green: level, blue: 0.4, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        commands.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
        if recorded {
            let index = frames.add(committed: CACurrentMediaTime(), target: target)
            let record = frames
            drawable.addPresentedHandler { record.presented(index, at: $0.presentedTime) }
        }
        if target == 0 || mode == .metalLink {
            commands.present(drawable)
        } else {
            commands.present(drawable, atTime: target)
        }
        paths.watch(commands)
        commands.commit()
    }
}

/// The drawables of a phase, written by the main thread at commit and by Metal on whatever thread reports them.
final class PresentedFrames: @unchecked Sendable {
    private let lock = NSLock()
    private var frames: [PresentTimeVerdict.Frame] = []

    func add(committed: Double, target: Double) -> Int {
        lock.lock()
        defer { lock.unlock() }
        frames.append(PresentTimeVerdict.Frame(committed: committed, target: target))
        return frames.count - 1
    }

    func presented(_ index: Int, at time: Double) {
        let handled = CACurrentMediaTime()
        lock.lock()
        defer { lock.unlock() }
        let f = frames[index]
        frames[index] = PresentTimeVerdict.Frame(
            committed: f.committed, target: f.target, presented: time, handled: handled)
    }

    var all: [PresentTimeVerdict.Frame] {
        lock.lock()
        defer { lock.unlock() }
        return frames
    }
}
