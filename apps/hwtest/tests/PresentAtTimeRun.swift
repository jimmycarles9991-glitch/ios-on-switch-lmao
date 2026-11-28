import HWTestCore
import HWTestGPU
import HWTestKit
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
    /// Plain presents and linkTarget's timed ones in turn on one layer: the plain ones' presented reports keep the
    /// layer's last report recent, which QuartzCore's present path checks (a layer with none for a second has every
    /// drawable it holds finished at once, with no presented time).
    case alternate
    /// presentDrawable:atTime: with targets that cycle through `sweepTargets`, each a kind of time QuartzCore may treat
    /// apart.
    case sweep

    /// The sweep's targets, in turn: under QuartzCore's half-millisecond floor (a plain present), just over it, one
    /// second after boot, ten seconds and a tenth of a second before the commit, and four periods after it.
    static let sweepTargets: [(name: String, target: (Double) -> Double)] = [
        ("floor", { _ in 0.0004 }), ("over", { _ in 0.001 }), ("boot", { _ in 1 }), ("past10", { $0 - 10 }),
        ("past01", { $0 - 0.1 }), ("ahead", { $0 + 4 * PresentTimeVerdict.period }),
    ]
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

    /// Resumed once every drawable recorded so far has had its presented handler.
    func everyPresented() -> Waiter<Bool> { frames.everyPresented() }

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
            case .alternate: target = tick % 8 == 0 ? 0 : link.targetTimestamp + 3 * period
            case .sweep: target = PresentMode.sweepTargets[(tick / 4) % PresentMode.sweepTargets.count]
                .target(committed)
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
    private var unreported = 0
    private var drained: Waiter<Bool>?

    func add(committed: Double, target: Double) -> Int {
        lock.lock()
        defer { lock.unlock() }
        frames.append(PresentTimeVerdict.Frame(committed: committed, target: target))
        unreported += 1
        return frames.count - 1
    }

    func presented(_ index: Int, at time: Double) {
        let handled = CACurrentMediaTime()
        lock.lock()
        let f = frames[index]
        frames[index] = PresentTimeVerdict.Frame(
            committed: f.committed, target: f.target, presented: time, handled: handled
        )
        unreported -= 1
        let waiter = unreported <= 0 ? drained : nil
        if waiter != nil { drained = nil }
        lock.unlock()
        waiter?.resume(true)
    }

    func everyPresented() -> Waiter<Bool> {
        let waiter = Waiter<Bool>()
        lock.lock()
        let done = unreported <= 0
        if !done { drained = waiter }
        lock.unlock()
        if done { waiter.resume(true) }
        return waiter
    }

    var all: [PresentTimeVerdict.Frame] {
        lock.lock()
        defer { lock.unlock() }
        return frames
    }
}
