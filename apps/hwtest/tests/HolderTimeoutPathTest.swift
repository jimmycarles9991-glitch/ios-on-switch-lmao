import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// A command buffer held on its own app's shared event that nobody signals fails as Metal's GPU fails it
/// (TegraGM20BMetalEvents.mm heldTooLong, predicate event.timeout), measured on an M4 Max with validation off (c38-audit ds3
/// probes/holder-timeout/MEASURED.txt: 5004.6-5005.5 ms over twelve runs, from the commit of a buffer that is its
/// queue's head). On the test's queue: the held buffer H fills word 0, waits for value 1 of a fresh event, fills word
/// 1; B1 and B2, committed behind it, fill words of their own; on a second queue one buffer fills its word at once.
/// Judged: held_ms in [5000, 5100] (no failure by 10 s is "never failed"), held_status 5 with held_code 2 (Timeout),
/// Metal's description and the IOGPUCommandQueueErrorDomain 2 error under it, word 0 written and word 1 not (the work
/// before the wait ran, after it not), B1 then B2 completed with their words, each within 50 ms of the failure, the
/// other queue's buffer within 100 ms of its commit (Apple: 4 ms), and, once the event reads 1, a buffer waiting on it
/// completes and writes (late_wait). In time: a second held buffer whose event the test signals 1 s after its commit
/// completes with both words. failed_event_timeout (switchMetalCounters failed.event.timeout over the test, debug builds)
/// is 1. waitUntilScheduled, called on a thread of its own right after each commit, returns at the failure for H
/// (held_scheduled_ms in [5000, 5100]; Apple 5004.8-5004.9 ms) and no sooner than the signal for the in-time hold
/// (in_time_scheduled_after_signal_ms >= 0). held_status_at_1s is reported (Apple: committed).
struct HolderTimeoutPathTest: HWTest {
    let id = "gpu.path.holder_timeout"
    let name = "A command buffer held on its own unsignalled event fails with Metal's timeout after 5 s"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 45 }

    static let boundMs = 5000.0 ... 5100.0
    static let neverFailedSeconds = 10.0, behindMs = 50.0, otherQueueMs = 100.0, inTimeSeconds = 1.0
    static let description = "Caused GPU Timeout Error (00000002:kIOGPUCommandBufferCallbackErrorTimeout)"
    static let words = 8

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        guard let other = gpu.device.makeCommandQueue(), let event = gpu.device.makeSharedEvent(),
              let inTimeEvent = gpu.device.makeSharedEvent(),
              let words = gpu.device.makeBuffer(length: Self.words * 4, options: .storageModeShared),
              let otherCommands = other.makeCommandBuffer()
        else { throw TestFailure("no second queue, shared event, buffer or command buffer") }
        memset(words.contents(), 0, Self.words * 4)
        let before = GM20BFacade.counters(of: gpu.device)
        let order = Order()
        let held = Ended(), behind = [Ended(), Ended()], otherEnded = Ended(), late = Ended(), inTime = Ended()

        let (heldCommands, heldCommitted) = try Self.committed(gpu, held) { commands in
            Self.fill(commands, words, 0)
            commands.encodeWaitForEvent(event, value: 1)
            Self.fill(commands, words, 1)
        }
        let heldScheduled = Self.scheduled(heldCommands)
        for (i, ended) in behind.enumerated() {
            _ = try Self.committed(gpu, ended) { commands in
                Self.fill(commands, words, 2 + i)
                commands.addCompletedHandler { _ in order.add(i + 1) }
            }
        }
        Self.fill(otherCommands, words, 4)
        otherCommands.addCompletedHandler { done in otherEnded.finish(done.status) }
        let otherCommitted = Self.nowMs()
        otherCommands.commit()
        let statusAt1s = Waiter<Int>()
        DispatchQueue.global().asyncAfter(deadline: .now() + 1) {
            statusAt1s.resume(Int(heldCommands.status.rawValue))
        }

        let otherMs = await otherEnded.waiter.wait(timeout: Self.neverFailedSeconds).map { $0 - otherCommitted }
        let heldEnd = await held.waiter.wait(timeout: Self.neverFailedSeconds)
        if heldEnd == nil { event.signaledValue = 1 }
        var behindEnds: [Double?] = []
        for ended in behind { behindEnds.append(await ended.waiter.wait(timeout: Self.neverFailedSeconds)) }
        event.signaledValue = 1
        _ = try Self.committed(gpu, late) { commands in
            commands.encodeWaitForEvent(event, value: 1)
            Self.fill(commands, words, 5)
        }
        let lateEnd = await late.waiter.wait(timeout: Self.neverFailedSeconds)
        let (inTimeCommands, _) = try Self.committed(gpu, inTime) { commands in
            Self.fill(commands, words, 6)
            commands.encodeWaitForEvent(inTimeEvent, value: 1)
            Self.fill(commands, words, 7)
        }
        let inTimeScheduled = Self.scheduled(inTimeCommands)
        let signalled = Waiter<Double>()
        DispatchQueue.global().asyncAfter(deadline: .now() + Self.inTimeSeconds) {
            let at = Self.nowMs()
            inTimeEvent.signaledValue = 1
            signalled.resume(at)
        }
        let inTimeEnd = await inTime.waiter.wait(timeout: Self.neverFailedSeconds)
        let heldScheduledMs = await heldScheduled.wait(timeout: Self.neverFailedSeconds).map { $0 - heldCommitted }
        let inTimeSignal = await signalled.wait(timeout: Self.neverFailedSeconds)
        let inTimeScheduledAt = await inTimeScheduled.wait(timeout: Self.neverFailedSeconds)
        let afterSignalMs = inTimeSignal.flatMap { signal in inTimeScheduledAt.map { $0 - signal } }
        let after = GM20BFacade.counters(of: gpu.device)

        let written = (0 ..< Self.words).map { Self.written(words, $0) }
        let error = heldCommands.error as NSError?
        let underlying = error?.userInfo[NSUnderlyingErrorKey] as? NSError
        let heldMs = heldEnd.map { $0 - heldCommitted }
        let heldStatus = Int(heldCommands.status.rawValue)
        let behindAfterMs = behindEnds.map { end in end.flatMap { e in heldEnd.map { e - $0 } } }
        let behindRan = order.text == "1,2" && behind.allSatisfy { $0.status == .completed } && written[2] && written[3]
        let lateRan = lateEnd != nil && late.status == .completed && written[5]
        let inTimeRan = inTimeEnd != nil && inTimeCommands.status == .completed && written[6] && written[7]
        let timeoutKey = "failed.event.timeout"
        let timedOut: Int? = after.isEmpty ? nil : (after[timeoutKey] ?? 0) - (before[timeoutKey] ?? 0)
        var metrics = Metrics()
        metrics.set("held_ms", heldMs ?? -1, digits: 1)
        metrics.set("held_status", heldStatus)
        metrics.set("held_code", error?.code ?? -1)
        metrics.set("held_description", error?.localizedDescription ?? "-")
        metrics.set("underlying_domain", underlying?.domain ?? "-")
        metrics.set("underlying_code", underlying?.code ?? -1)
        metrics.set("word0", written[0])
        metrics.set("word1", written[1])
        metrics.set("behind_order", order.text)
        metrics.set("behind_status", behind.map { String($0.status.rawValue) }.joined(separator: ","))
        let behindText = behindAfterMs.map { $0.map { String(format: "%.1f", $0) } ?? "-" }
        metrics.set("behind_after_ms", behindText.joined(separator: ","))
        metrics.set("queue_b_ms", otherMs ?? -1, digits: 1)
        metrics.set("late_wait", lateRan)
        metrics.set("in_time", inTimeRan)
        metrics.set("held_status_at_1s", await statusAt1s.wait(timeout: Self.neverFailedSeconds) ?? -1)
        metrics.set("held_scheduled_ms", heldScheduledMs ?? -1, digits: 1)
        metrics.set("in_time_scheduled_after_signal_ms", afterSignalMs ?? -1, digits: 1)
        metrics.set("failed_event_timeout", timedOut ?? -1)
        let timeoutError = error?.domain == MTLCommandBufferErrorDomain && error?.code == 2
        let ioError = underlying?.domain == "IOGPUCommandQueueErrorDomain" && underlying?.code == 2
        var checks: [(String, Bool)] = [
            ("the held buffer failed within \(Int(Self.neverFailedSeconds)) s", heldMs != nil),
            ("held_ms in [5000, 5100] (\(Int(heldMs ?? -1)))", heldMs.map { Self.boundMs.contains($0) } ?? false),
            ("held_status 5 (error)", heldStatus == Int(MTLCommandBufferStatus.error.rawValue)),
            ("held_code 2 (MTLCommandBufferErrorTimeout)", timeoutError),
            ("held_description is Metal's", error?.localizedDescription == Self.description),
            ("IOGPUCommandQueueErrorDomain 2 under it", ioError),
            ("word 0 (before the wait) written", written[0]),
            ("word 1 (after the wait) not written", !written[1]),
            ("B1 then B2 completed behind it, their words written", behindRan),
            (
                "each behind it within \(Int(Self.behindMs)) ms of the failure",
                behindAfterMs.allSatisfy { ($0 ?? .infinity) <= Self.behindMs }
            ),
            (
                "the other queue's buffer within \(Int(Self.otherQueueMs)) ms",
                (otherMs ?? .infinity) < Self.otherQueueMs
            ),
            ("a later wait on the signalled value completes and writes", lateRan),
            ("a hold signalled in time completes with both words", inTimeRan),
            (
                "H's waitUntilScheduled returned at its failure (\(Int(heldScheduledMs ?? -1)) ms)",
                heldScheduledMs.map { Self.boundMs.contains($0) } ?? false
            ),
            ("the in-time hold's waitUntilScheduled returned no sooner than its signal", (afterSignalMs ?? -1) >= 0),
        ]
        if let timedOut { checks.append(("failed.event.timeout counted once (\(timedOut))", timedOut == 1)) }
        return Outcome.judged(checks, metrics, proof: gpu.proof).onRawPath(gpu, expecting: ["event.timeout": 1])
    }

    /// A buffer of the test's queue (gpu.commit) and its commit time; its completion time and status kept in `ended`
    private static func committed(
        _ gpu: GPU, _ ended: Ended, _ encode: (MTLCommandBuffer) -> Void
    ) throws -> (MTLCommandBuffer, Double) {
        var at = 0.0
        let commands = try gpu.commit { commands in
            encode(commands)
            commands.addCompletedHandler { done in ended.finish(done.status) }
            at = nowMs()
        }
        return (commands, at)
    }

    /// When `commands`' waitUntilScheduled, called on a thread of its own now, returned (nowMs)
    private static func scheduled(_ commands: MTLCommandBuffer) -> Waiter<Double> {
        let returned = Waiter<Double>()
        Thread {
            commands.waitUntilScheduled()
            returned.resume(nowMs())
        }.start()
        return returned
    }

    static func fill(_ commands: MTLCommandBuffer, _ words: MTLBuffer, _ word: Int) {
        let blit = commands.makeBlitCommandEncoder()!
        blit.fill(buffer: words, range: (word * 4) ..< (word * 4 + 4), value: UInt8(word + 1))
        blit.endEncoding()
    }

    static func written(_ words: MTLBuffer, _ word: Int) -> Bool {
        UnsafeRawBufferPointer(start: words.contents() + word * 4, count: 4).allSatisfy { $0 == UInt8(word + 1) }
    }

    static func nowMs() -> Double { Double(DispatchTime.now().uptimeNanoseconds) / 1e6 }
}

/// A command buffer's completion: when (the waiter's value, in nowMs) and its status then
private final class Ended: @unchecked Sendable {
    let waiter = Waiter<Double>()
    private let lock = NSLock()
    private var ended = MTLCommandBufferStatus.notEnqueued

    func finish(_ status: MTLCommandBufferStatus) {
        lock.lock()
        ended = status
        lock.unlock()
        waiter.resume(HolderTimeoutPathTest.nowMs())
    }

    var status: MTLCommandBufferStatus {
        lock.lock()
        defer { lock.unlock() }
        return ended
    }
}

private final class Order: @unchecked Sendable {
    private let lock = NSLock()
    private var order: [Int] = []

    func add(_ i: Int) {
        lock.lock()
        order.append(i)
        lock.unlock()
    }

    var text: String {
        lock.lock()
        defer { lock.unlock() }
        return order.map(String.init).joined(separator: ",")
    }
}
