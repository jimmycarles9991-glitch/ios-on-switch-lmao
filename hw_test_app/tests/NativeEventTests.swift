import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The raw facade's event jobs (drivers/TegraGM20BMetal/raw/runtime/RawMetalEventJobs.mm, debug runtimes): jobs of no
/// work it made, by what they carried, and the signals whose value the CPU set after the work before them.
enum EventJobs {
    static func read(_ device: MTLDevice) -> [String: Int] {
        let selector = NSSelectorFromString("switchRawEvents")
        guard let device = device as? NSObject, device.responds(to: selector),
            let answer = device.perform(selector)?.takeUnretainedValue() as? [String: NSNumber]
        else { return [:] }
        return answer.mapValues(\.intValue)
    }
}

/// A value a handler sets on another thread, read once the handler signalled it ran
final class HandlerValue<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var held: T

    init(_ value: T) { held = value }

    var value: T {
        get { lock.withLock { held } }
        set { lock.withLock { held = newValue } }
    }
}

/// Events: every event the device makes is a kernel event, plain ones too. A command buffer of a signal alone and a
/// signal after an encoder m2v ran each go to the GPU in a job of no work; a listener on an event made from the first's
/// handle hears its GPU signal; a plain event orders another queue's copy after a fill; a command buffer that ends in a
/// wait completes (where its producer stood then is logged, the two completions being unordered). No signal's value is
/// set by the CPU.
struct NativeSharedEventTest: NativeObjectTest {
    let id = "metal.native.shared_event"
    let name = "Events are the device's own: GPU signals with no work, handles, listeners, GPU waits"

    static func completion(of buffer: MTLCommandBuffer) -> DispatchSemaphore {
        let done = DispatchSemaphore(value: 0)
        buffer.addCompletedHandler { _ in done.signal() }
        return done
    }

    static func waited(_ semaphore: DispatchSemaphore, seconds: Double) async -> Bool {
        await withCheckedContinuation { continuation in
            DispatchQueue.global().async {
                continuation.resume(returning: semaphore.wait(timeout: .now() + seconds) == .success)
            }
        }
    }

    /// Whether the event reaches `value` within a second (MTLSharedEvent's own wait, off the test's task)
    static func reached(_ event: MTLSharedEvent, _ value: UInt64) async -> Bool {
        let held = HandlerValue<MTLSharedEvent?>(event)
        return await withCheckedContinuation { continuation in
            DispatchQueue.global().async {
                continuation.resume(returning: held.value?.wait(untilSignaledValue: value, timeoutMS: 1000) ?? false)
            }
        }
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        guard let plain = device.makeEvent(), let shared = device.makeSharedEvent(),
            let other = device.makeCommandQueue()
        else { throw TestFailure("no event or second queue") }
        guard let alias = device.makeSharedEvent(handle: shared.makeSharedEventHandle()) else {
            throw TestFailure("no event from the shared event's handle")
        }
        var work = NativeWork(objects: [plain, shared, alias])
        let before = EventJobs.read(device)
        if (shared.device as AnyObject?) !== (device as AnyObject) { work.failures.append("the event names no device") }

        let alone = try await gpu.run { $0.encodeSignalEvent(shared, value: 1) }
        work.paths.append(alone.path)
        metrics.set("value_at_completion", Int(shared.signaledValue))
        if !(await Self.reached(shared, 1)) {
            work.failures.append("a command buffer of a signal alone never reached its value")
        }

        let heard = DispatchSemaphore(value: 0)
        let reached = HandlerValue<UInt64>(0)
        alias.notify(MTLSharedEventListener(dispatchQueue: DispatchQueue(label: "hwtest.native.listener")), atValue: 2)
        {
            _, value in
            reached.value = value
            heard.signal()
        }
        let length = 1 << 16
        let source = try gpu.buffer(length), target = try gpu.buffer(length)
        let filled = try await gpu.run { commands in
            let fill = commands.makeBlitCommandEncoder()!
            fill.fill(buffer: source, range: 0..<length, value: 0x3c)
            fill.endEncoding()
            commands.encodeSignalEvent(shared, value: 2)
        }
        work.paths.append(filled.path)
        let listened = await Self.waited(heard, seconds: 2)
        metrics.set("listener_value", Int(reached.value))
        if !listened { work.failures.append("the handle's event never told its listener of the GPU signal") }

        guard let waiter = other.makeCommandBuffer(), let producer = gpu.queue.makeCommandBuffer() else {
            throw TestFailure("no command buffers")
        }
        waiter.encodeWaitForEvent(plain, value: 1)
        let copy = waiter.makeBlitCommandEncoder()!
        copy.copy(from: source, sourceOffset: 0, to: target, destinationOffset: 0, size: length)
        copy.endEncoding()
        let copied = Self.completion(of: waiter)
        waiter.commit()
        let fill = producer.makeBlitCommandEncoder()!
        fill.fill(buffer: source, range: 0..<length, value: 0x5a)
        fill.endEncoding()
        producer.encodeSignalEvent(plain, value: 1)
        producer.commit()
        if !(await Self.waited(copied, seconds: 5)) {
            work.failures.append("the copy waiting on a plain event never ran")
        }
        work.paths += [RawFacade.path(of: waiter), RawFacade.path(of: producer)]
        let wrong = UnsafeRawBufferPointer(start: target.contents(), count: length).filter { $0 != 0x5a }.count
        metrics.set("wrong_bytes", wrong)
        if wrong != 0 { work.failures.append("\(wrong) copied bytes miss the fill the event signalled") }

        guard let tail = other.makeCommandBuffer(), let late = gpu.queue.makeCommandBuffer() else {
            throw TestFailure("no command buffers")
        }
        tail.encodeWaitForEvent(shared, value: 3)
        let lateAtTail = HandlerValue(MTLCommandBufferStatus.notEnqueued)
        let producerOfTail = HandlerValue<MTLCommandBuffer?>(late)
        tail.addCompletedHandler { _ in lateAtTail.value = producerOfTail.value?.status ?? .notEnqueued }
        let tailDone = Self.completion(of: tail)
        tail.commit()
        let lateFill = late.makeBlitCommandEncoder()!
        lateFill.fill(buffer: target, range: 0..<length, value: 0x11)
        lateFill.endEncoding()
        late.encodeSignalEvent(shared, value: 3)
        late.commit()
        if !(await Self.waited(tailDone, seconds: 5)) {
            work.failures.append("a command buffer ending in a wait never completed")
        }
        work.paths += [RawFacade.path(of: tail), RawFacade.path(of: late)]
        metrics.set("producer_at_waiter", Int(lateAtTail.value.rawValue))

        let kernel = try gpu.compute("fma_loop")
        let data = try gpu.buffer(64 * 16)
        let arguments = try gpu.buffer(16)
        arguments.contents().storeBytes(of: SIMD3<UInt32>(1, 1, 1), as: SIMD3<UInt32>.self)
        let mixed = try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(kernel)
            encoder.setBuffer(data, offset: 0, index: 0)
            var iterations: UInt32 = 4
            encoder.setBytes(&iterations, length: 4, index: 1)
            encoder.dispatchThreadgroups(
                indirectBuffer: arguments, indirectBufferOffset: 0,
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
            encoder.endEncoding()
            commands.encodeSignalEvent(shared, value: 4)
        }
        metrics.set("after_m2v_path", mixed.path)
        if !(await Self.reached(shared, 4)) {
            work.failures.append("a signal after an m2v encoder never reached its value")
        }

        let after = EventJobs.read(device)
        for key in ["signal_jobs", "cpu_signals"] {
            metrics.set(key, (after[key] ?? 0) - (before[key] ?? 0))
        }
        if (after["cpu_signals"] ?? 0) != (before["cpu_signals"] ?? 0) {
            work.failures.append("the CPU set a signal's value")
        }
        if (after["signal_jobs"] ?? 0) <= (before["signal_jobs"] ?? 0) { work.failures.append("no signal job") }
        return work
    }
}
