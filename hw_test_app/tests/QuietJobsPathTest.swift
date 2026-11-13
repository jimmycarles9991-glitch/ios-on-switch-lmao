import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Quiet job ends (drivers/TegraGM20B/SwitchGpuQuiet.h, raw/runtime/raw_completion_source.h): a frame of three command
/// buffers with no handlers committed back to back and a fourth with a completed handler ends with one interrupt, the
/// fourth's; the others' jobs end quiet. The interrupts a frame costs are counted as its jobs that interrupt
/// (submit_jobs - quiet_jobs) and the wake entries waits asked for (wake_asks). Then a waiter of a quiet job with nothing
/// submitted behind it (the next command buffer waits for a shared event) asks the kernel for its wake entry and is
/// woken. A wake that never came is the kernel's one-line panic ("... was done but its wake never came"), not a failure
/// here.
struct QuietJobsPathTest: GPUPathTest {
    let id = "gpu.path.quiet_jobs"
    let name = "Command buffers nobody waits for alone end without an interrupt, and their waiters still wake"
    let counters = ["quiet_jobs", "wake_asks"]
    static let frames = 12, perFrame = 4, size = 16, interruptsBound = 2.0, wakeBoundMs = 1000.0

    static func colour(_ frame: Int, _ index: Int) -> [UInt8] { [UInt8(frame * 20), UInt8(index * 60), 0x40, 255] }

    func clear(_ commands: MTLCommandBuffer, _ target: MTLTexture, _ colour: [UInt8]) {
        let pass = MTLRenderPassDescriptor()
        let c = colour.map { Double($0) / 255 }
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: c[0], green: c[1], blue: c[2], alpha: c[3])
        pass.colorAttachments[0].storeAction = .store
        commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
    }

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let connection = GPUConnection()
        guard case .counts(let before) = GPUPaths.read(connection) else { throw TestFailure("no path counters") }
        var failures: [String] = []
        var targets: [MTLTexture] = []
        var all: [MTLCommandBuffer] = []
        var handled = 0
        let lock = NSLock()
        let done = DispatchSemaphore(value: 0)
        // Each frame's buffers are enqueued in order and committed last first, so the first is taken with the rest
        // behind it; no thread waits in waitUntilCompleted meanwhile, which would ask for every range's interrupt.
        for frame in 0..<Self.frames {
            var frameBuffers: [MTLCommandBuffer] = []
            for index in 0..<Self.perFrame {
                let target = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
                targets.append(target)
                guard let commands = gpu.queue.makeCommandBuffer() else { throw TestFailure("no command buffer") }
                clear(commands, target, Self.colour(frame, index))
                if index + 1 == Self.perFrame {
                    commands.addCompletedHandler { _ in
                        lock.lock()
                        handled += 1
                        let last = handled == Self.frames
                        lock.unlock()
                        if last { done.signal() }
                    }
                }
                commands.enqueue()
                frameBuffers.append(commands)
            }
            for commands in frameBuffers.reversed() { commands.commit() }
            all += frameBuffers
        }
        if done.wait(timeout: .now() + 10) == .timedOut {
            failures.append("the frames' completed handlers never all ran")
        }
        try gpu.finish(all)
        guard case .counts(let framesDone) = GPUPaths.read(connection) else {
            throw TestFailure("no path counters after the frames")
        }
        let jobs = framesDone.taken("submit_jobs", since: before)
        let quiet = framesDone.taken("quiet_jobs", since: before)
        let asks = framesDone.taken("wake_asks", since: before)
        let interrupts = Double(jobs - quiet + asks) / Double(Self.frames)
        metrics.set("frames", Self.frames)
        metrics.set("jobs_per_frame", String(format: "%.2f", Double(jobs) / Double(Self.frames)))
        metrics.set("interrupts_per_frame", String(format: "%.2f", interrupts))
        if interrupts > Self.interruptsBound {
            failures.append(String(format: "%.2f interrupts a frame (at most %.1f)", interrupts, Self.interruptsBound))
        }
        lock.lock()
        let handlers = handled
        lock.unlock()
        if handlers != Self.frames { failures.append("\(handlers) of \(Self.frames) completed handlers ran") }
        var wrong = 0
        for (at, target) in targets.enumerated() {
            let bytes = try await gpu.read(target, bytesPerPixel: 4)
            let want = Self.colour(at / Self.perFrame, at % Self.perFrame)
            for pixel in stride(from: 0, to: bytes.count, by: 4) {
                wrong += Self.wrong(Array(bytes[pixel..<(pixel + 4)]), want)
            }
        }
        metrics.set("wrong_bytes", wrong)
        if wrong != 0 { failures.append("\(wrong) bytes differ from the clears") }
        failures += try waiterOfAQuietJob(gpu, before: framesDone, connection, &metrics)
        return failures
    }

    /// A quiet job whose follower parks on a shared event: the completion thread's wait asks for the wake entry.
    func waiterOfAQuietJob(
        _ gpu: GPU, before: GPUPaths, _ connection: GPUConnection?, _ metrics: inout Metrics
    ) throws
        -> [String]
    {
        guard let event = gpu.device.makeSharedEvent(), let first = gpu.queue.makeCommandBuffer(),
            let second = gpu.queue.makeCommandBuffer()
        else { throw TestFailure("no shared event or command buffer") }
        let target = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
        clear(first, target, [1, 2, 3, 255])
        second.encodeWaitForEvent(event, value: 1)
        clear(second, try gpu.texture(.rgba8Unorm, Self.size, Self.size), [4, 5, 6, 255])
        first.enqueue()
        second.enqueue()
        second.commit()
        first.commit()
        first.waitUntilScheduled()
        let start = Date()
        first.waitUntilCompleted()
        let waitedMs = Date().timeIntervalSince(start) * 1000
        event.signaledValue = 1
        second.waitUntilCompleted()
        var failures: [String] = []
        metrics.set("quiet_wait_ms", String(format: "%.1f", waitedMs))
        if first.status != .completed {
            failures.append("the quiet job's command buffer ended \(first.status.rawValue)")
        }
        if waitedMs > Self.wakeBoundMs {
            failures.append(String(format: "its waiter woke after %.0f ms (at most %.0f)", waitedMs, Self.wakeBoundMs))
        }
        if case .counts(let after) = GPUPaths.read(connection) {
            let asks = after.taken("wake_asks", since: before)
            metrics.set("quiet_waiter_asks", asks)
            if asks < 1 { failures.append("its waiter asked for no wake entry: the job interrupted after all") }
        }
        return failures
    }
}
