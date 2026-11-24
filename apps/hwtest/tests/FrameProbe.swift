import Foundation
import Metal
import QuartzCore
import UIKit

/// A small CAMetalLayer that presents a cleared drawable at every frame of a display link on a thread of its own, so it
/// keeps presenting while a test's main thread is busy or blocked, and keeps each drawable's commit and presented time
/// (CACurrentMediaTime's clock; 0: not shown). Its presented times are the compositor's and the display's frames.
final class FrameProbe: NSObject, @unchecked Sendable {
    struct Frame {
        let committed: Double
        var presented: Double
    }

    let layer = CAMetalLayer()
    private let queue: MTLCommandQueue?
    private let lock = NSLock()
    private var frames: [Frame] = []
    private var link: CADisplayLink?
    private var thread: Thread?
    private var level = 0
    private var stopped = Waiter<Bool>()

    @MainActor
    init(frame: CGRect) {
        let device = MTLCreateSystemDefaultDevice()
        queue = device?.makeCommandQueue()
        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        layer.framebufferOnly = true
        layer.frame = frame
        layer.contentsScale = UIScreen.main.scale
    }

    /// Starts the probe's thread, which presents from its next display-link frame on; again after `stop`, the frames
    /// kept. QuartzCore finishes the first drawable a layer presents after a second with no presented report at once,
    /// with no presented time (24B119 layer_private_present_impl), so a restart's first frame has none.
    func start() {
        let stopped = Waiter<Bool>()
        self.stopped = stopped
        let thread = Thread { [self] in run(stopped) }
        thread.qualityOfService = .userInteractive
        thread.name = "FrameProbe"
        self.thread = thread
        thread.start()
    }

    /// Ends the probe's display link and waits for its thread to leave its run loop.
    func stop() async {
        guard let thread else { return }
        self.thread = nil
        perform(#selector(end), on: thread, with: nil, waitUntilDone: false)
        _ = await stopped.wait(timeout: 2)
    }

    /// Every frame since `start`, in commit order.
    var all: [Frame] {
        lock.lock()
        defer { lock.unlock() }
        return frames
    }

    /// The presented times of the frames committed in [from, to] (0 for a frame not shown).
    func presented(from: Double, to: Double) -> [Double] {
        all.filter { $0.committed >= from && $0.committed <= to }.map(\.presented)
    }

    /// When the first frame committed at or after `time` was shown (0: not shown, or none committed yet).
    func shown(after time: Double) -> Double {
        all.first { $0.committed >= time }?.presented ?? 0
    }

    private func run(_ stopped: Waiter<Bool>) {
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        link.add(to: .current, forMode: .default)
        self.link = link
        CFRunLoopRun()
        stopped.resume(true)
    }

    @objc private func end() {
        link?.invalidate()
        link = nil
        CFRunLoopStop(CFRunLoopGetCurrent())
    }

    @objc private func tick(_ link: CADisplayLink) {
        guard let queue, let drawable = layer.nextDrawable(), let commands = queue.makeCommandBuffer() else { return }
        level = (level + 1) % 60
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = drawable.texture
        pass.colorAttachments[0].loadAction = .clear
        let value = Double(level) / 60
        pass.colorAttachments[0].clearColor = MTLClearColor(red: value, green: 0.5, blue: 1 - value, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        commands.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
        lock.lock()
        let index = frames.count
        frames.append(Frame(committed: CACurrentMediaTime(), presented: 0))
        lock.unlock()
        drawable.addPresentedHandler { [weak self] shown in self?.shown(index, at: shown.presentedTime) }
        commands.present(drawable)
        commands.commit()
    }

    private func shown(_ index: Int, at time: Double) {
        lock.lock()
        defer { lock.unlock() }
        frames[index].presented = time
    }
}
