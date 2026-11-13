import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A shared event's signal in the middle of a raw command buffer (drivers/TegraGM20BMetal/raw/runtime/RawMetalInlineSignals.mm
/// and the kernel's event words, TegraGM20BJobABI.h method 40): a compute dispatch and a fill, the signal, then compute work
/// four times longer, as one job whose pushes release the signal into the event's word. A listener must hear the value
/// before the command buffer completes, and a command buffer committed right after it on another queue, waiting for the
/// value while the work before the signal still runs (the kernel's events line counts its wait on the word), must copy the
/// fill. Reports the command buffer's jobs alone (the facade's jobs counter: one, where the signal
/// used to end a job and start another), the signals released mid-job, and how long after the work before the signal the
/// listener heard it. The rounds after those write only private memory before the signal, so on a kernel with addressed
/// jobs the release goes without the L2 flush (raw_event_release.h), and the waiter must still copy the fill:
/// unflushed_signals counts them (0 where exact_jobs=0).
struct EventReleaseTest: HWTest {
    let id = "gpu.raw.event_release"
    let name = "A mid-command-buffer shared-event signal: one job, heard before the command buffer completes"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let threads = 1 << 14
    static let bytes = 1 << 16
    static let rounds = 3
    static let longMilliseconds = 12.0

    struct Work {
        let gpu: GPU
        let kernel: MTLComputePipelineState
        let data: MTLBuffer
        let source: MTLBuffer

        func dispatch(_ commands: MTLCommandBuffer, iterations: UInt32) {
            var count = iterations
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(kernel)
            encoder.setBuffer(data, offset: 0, index: 0)
            encoder.setBytes(&count, length: 4, index: 1)
            encoder.dispatchThreads(
                MTLSize(width: EventReleaseTest.threads, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
            encoder.endEncoding()
        }

        /// The work before the signal: a dispatch, then the fill the waiter copies
        func before(_ commands: MTLCommandBuffer, fill: UInt8, iterations: UInt32) {
            dispatch(commands, iterations: iterations)
            let blit = commands.makeBlitCommandEncoder()!
            blit.fill(buffer: source, range: 0..<EventReleaseTest.bytes, value: fill)
            blit.endEncoding()
        }
    }

    struct Round {
        var path = ""
        var jobs = 0, released = 0, signalJobs = 0
        var heardBeforeEnd = false
        var commitToHeardMs = 0.0, heardBeforeEndMs = 0.0
        var waiterDone = false, waiterWrong = 0
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        guard EventJobs.read(gpu.device)["jobs"] != nil else {
            return .skip("the facade counts no jobs (release runtime)")
        }
        guard let event = gpu.device.makeSharedEvent(), let other = gpu.device.makeCommandQueue() else {
            throw TestFailure("no shared event or second queue")
        }
        let work = Work(
            gpu: gpu, kernel: try gpu.compute("fma_loop"),
            data: try gpu.buffer((0..<Self.threads * 4).map { Float($0 % 7) * 0.001 }),
            source: try gpu.buffer(Self.bytes))
        var metrics = Metrics()
        var wrong: [String] = []

        let probe = try await gpu.run { work.dispatch($0, iterations: 4096) }
        let probeMs = probe.gpuTimed ? probe.gpuMilliseconds : probe.wallMilliseconds
        let iterations = UInt32(4096 * min(8.0, max(1.0, Self.longMilliseconds / max(probeMs, 0.01))))
        let before = try await gpu.run { work.before($0, fill: 1, iterations: iterations / 4) }
        metrics.set("long_iterations", Int(iterations))
        metrics.set("before_wall_ms", before.wallMilliseconds, digits: 3)

        var rounds: [Round] = []
        for round in 0..<Self.rounds {
            let value = UInt64(round + 1)
            rounds.append(try await signalled(work, event, value, iterations, waiter: round == 0 ? nil : other))
        }
        guard let privateData = gpu.device.makeBuffer(length: Self.threads * 4, options: .storageModePrivate),
            let privateSource = gpu.device.makeBuffer(length: Self.bytes, options: .storageModePrivate)
        else { throw TestFailure("no private buffers") }
        let privateWork = Work(gpu: gpu, kernel: work.kernel, data: privateData, source: privateSource)
        let unflushedBefore = EventJobs.read(gpu.device)["unflushed_signals"] ?? 0
        for round in 0..<Self.rounds {
            let value = UInt64(Self.rounds + round + 1)
            rounds.append(try await signalled(privateWork, event, value, iterations, waiter: round == 0 ? nil : other))
        }
        let unflushed = (EventJobs.read(gpu.device)["unflushed_signals"] ?? 0) - unflushedBefore
        let solo = rounds[0]
        metrics.set("path", solo.path)
        if rounds.contains(where: { $0.path != "raw" }) {
            return .fail("path", metrics)
        }
        metrics.set("jobs_per_cb", solo.jobs)
        metrics.set("released_signals", rounds.map(\.released).reduce(0, +))
        metrics.set("signal_jobs", rounds.map(\.signalJobs).reduce(0, +))
        let latencies = rounds.map { $0.commitToHeardMs - before.wallMilliseconds }.sorted()
        metrics.set("notify_after_work_ms", latencies[latencies.count / 2], digits: 3)
        metrics.set("heard_before_end_ms", rounds.map(\.heardBeforeEndMs).min() ?? 0, digits: 3)
        metrics.set("waiter_wrong_bytes", rounds.map(\.waiterWrong).reduce(0, +))
        if solo.jobs != 1 { wrong.append("jobs_per_cb") }
        if rounds.contains(where: { $0.released != 1 }) { wrong.append("released") }
        if rounds.contains(where: { !$0.heardBeforeEnd }) { wrong.append("heard_after_end") }
        if rounds.dropFirst().contains(where: { !$0.waiterDone || $0.waiterWrong != 0 }) { wrong.append("waiter") }
        let exactJobs = (EventJobs.read(gpu.device)["exact_jobs"] ?? 0) != 0
        metrics.set("unflushed_signals", unflushed)
        metrics.set("exact_jobs", exactJobs ? 1 : 0)
        if unflushed != (exactJobs ? Self.rounds : 0) { wrong.append("unflushed_signals") }
        if event.signaledValue != UInt64(2 * Self.rounds) { wrong.append("final_value") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    /// One command buffer: the work before, the signal of `value`, the long work; a listener on `value`; with `waiter`,
    /// a command buffer on that queue committed right after it that waits for `value` and copies the fill
    func signalled(
        _ work: Work, _ event: MTLSharedEvent, _ value: UInt64, _ iterations: UInt32, waiter: MTLCommandQueue?
    ) async throws -> Round {
        let fill = UInt8(0x40 + value)
        let counters = EventJobs.read(work.gpu.device)
        let heard = HandlerValue<Double>(0), ended = HandlerValue<Double>(0)
        let heardSignal = DispatchSemaphore(value: 0)
        event.notify(
            MTLSharedEventListener(dispatchQueue: DispatchQueue(label: "hwtest.event_release")), atValue: value
        ) {
            _, _ in
            heard.value = CFAbsoluteTimeGetCurrent()
            heardSignal.signal()
        }
        guard let commands = work.gpu.queue.makeCommandBuffer() else { throw TestFailure("no command buffer") }
        work.before(commands, fill: fill, iterations: iterations / 4)
        commands.encodeSignalEvent(event, value: value)
        work.dispatch(commands, iterations: iterations)
        commands.addCompletedHandler { _ in ended.value = CFAbsoluteTimeGetCurrent() }
        let done = NativeSharedEventTest.completion(of: commands)
        let copy = try work.gpu.buffer(Self.bytes)
        let committed = CFAbsoluteTimeGetCurrent()
        commands.commit()
        var waited: DispatchSemaphore?
        if let waiter, let consumer = waiter.makeCommandBuffer() {
            consumer.encodeWaitForEvent(event, value: value)
            let blit = consumer.makeBlitCommandEncoder()!
            blit.copy(from: work.source, sourceOffset: 0, to: copy, destinationOffset: 0, size: Self.bytes)
            blit.endEncoding()
            waited = NativeSharedEventTest.completion(of: consumer)
            consumer.commit()
        }
        var round = Round()
        let completed = await NativeSharedEventTest.waited(done, seconds: 10)
        let listened = await NativeSharedEventTest.waited(heardSignal, seconds: 2)
        if let waited {
            round.waiterDone = await NativeSharedEventTest.waited(waited, seconds: 5)
            round.waiterWrong =
                UnsafeRawBufferPointer(start: copy.contents(), count: Self.bytes).filter { $0 != fill }.count
        }
        if !completed || commands.status != .completed { throw TestFailure("the signalled command buffer failed") }
        round.path = RawFacade.path(of: commands)
        let after = EventJobs.read(work.gpu.device)
        round.jobs = (after["jobs"] ?? 0) - (counters["jobs"] ?? 0) - (waiter == nil ? 0 : 1)
        round.released = (after["released_signals"] ?? 0) - (counters["released_signals"] ?? 0)
        round.signalJobs = (after["signal_jobs"] ?? 0) - (counters["signal_jobs"] ?? 0)
        round.heardBeforeEnd = listened && heard.value < ended.value
        round.commitToHeardMs = (heard.value - committed) * 1000
        round.heardBeforeEndMs = (ended.value - heard.value) * 1000
        return round
    }
}
