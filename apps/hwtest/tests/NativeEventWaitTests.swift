import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Event waits no raw work follows go to the GPU, not the scheduler: a command buffer of a wait then a signal relays one
/// queue's producer to a second event in a job of no work, a command buffer that ends in a wait completes (where its
/// producer stood then is logged: two queues' completions are not ordered), and a third queue's copy waits for the relayed
/// value before reading the producer's bytes. The producer runs two 64 MiB fills, so it is still on the GPU as the waits are submitted; no wait holds its
/// queue on the CPU and no signal's value is set by the CPU.
struct NativeEventWaitTest: NativeObjectTest {
    let id = "metal.native.event_waits"
    let name = "Event waits before signals, at a command buffer's end and before work are taken by the GPU"

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        guard let produced = device.makeSharedEvent(), let relayed = device.makeEvent(),
            let big = device.makeBuffer(length: 64 << 20, options: .storageModePrivate),
            let source = device.makeBuffer(length: 4096, options: .storageModeShared),
            let target = device.makeBuffer(length: 4096, options: .storageModeShared),
            let check = device.makeBuffer(length: 4096, options: .storageModeShared),
            let producerQueue = device.makeCommandQueue(), let waiterQueue = device.makeCommandQueue(),
            let readerQueue = device.makeCommandQueue()
        else { throw TestFailure("the device made no events, buffers or queues") }
        memset(source.contents(), 0x5a, 4096)
        memset(target.contents(), 0, 4096)
        memset(check.contents(), 0, 4096)
        var work = NativeWork(objects: [produced, relayed])
        let before = EventJobs.read(device)

        let producer = producerQueue.makeCommandBuffer()!
        let fills = producer.makeBlitCommandEncoder()!
        fills.fill(buffer: big, range: 0..<big.length, value: 1)
        fills.fill(buffer: big, range: 0..<big.length, value: 2)
        fills.copy(from: source, sourceOffset: 0, to: target, destinationOffset: 0, size: 4096)
        fills.endEncoding()
        producer.encodeSignalEvent(produced, value: 1)
        let producerDone = NativeSharedEventTest.completion(of: producer)
        producer.commit()
        producer.waitUntilScheduled()

        let relay = waiterQueue.makeCommandBuffer()!
        relay.encodeWaitForEvent(produced, value: 1)
        relay.encodeSignalEvent(relayed, value: 1)
        let tail = waiterQueue.makeCommandBuffer()!
        tail.encodeWaitForEvent(produced, value: 1)
        let producerAtTail = HandlerValue(MTLCommandBufferStatus.notEnqueued)
        tail.addCompletedHandler { _ in producerAtTail.value = producer.status }
        let tailDone = NativeSharedEventTest.completion(of: tail)
        let reader = readerQueue.makeCommandBuffer()!
        reader.encodeWaitForEvent(relayed, value: 1)
        let copy = reader.makeBlitCommandEncoder()!
        copy.copy(from: target, sourceOffset: 0, to: check, destinationOffset: 0, size: 4096)
        copy.endEncoding()
        let readerDone = NativeSharedEventTest.completion(of: reader)
        relay.commit()
        relay.waitUntilScheduled()
        tail.commit()
        reader.commit()
        let producerRunning = producer.status != .completed
        metrics.set("producer_running_at_commit", producerRunning ? 1 : 0)

        for (name, done) in [("producer", producerDone), ("tail", tailDone), ("reader", readerDone)]
        where !(await NativeSharedEventTest.waited(done, seconds: 5)) {
            work.failures.append("the \(name) command buffer never completed")
        }
        metrics.set(
            "producer_at_tail", producerAtTail.value == .completed ? "completed" : "\(producerAtTail.value.rawValue)")
        let wrong = (0..<4096).filter { check.contents().load(fromByteOffset: $0, as: UInt8.self) != 0x5a }.count
        metrics.set("wrong_bytes", wrong)
        if wrong != 0 { work.failures.append("\(wrong) bytes the relayed wait let through early") }
        work.paths = [relay, tail, reader].map { RawFacade.path(of: $0) }

        let after = EventJobs.read(device)
        for key in ["wait_jobs", "signal_jobs", "promise_waits", "cpu_waits", "cpu_signals"] {
            metrics.set(key, (after[key] ?? 0) - (before[key] ?? 0))
        }
        if (after["cpu_signals"] ?? 0) != (before["cpu_signals"] ?? 0) { work.failures.append("the CPU set a signal") }
        if producerRunning {
            if (after["cpu_waits"] ?? 0) != (before["cpu_waits"] ?? 0) {
                work.failures.append("a wait held its queue on the CPU")
            }
            if (after["wait_jobs"] ?? 0) - (before["wait_jobs"] ?? 0) < 2 {
                work.failures.append("fewer than two wait jobs")
            }
        }
        return work
    }
}
