import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The raw device's resource set (drivers/TegraGM20BMetal/raw/runtime/raw_resource_set.h) across boot 18's
/// sequence, where every command buffer after it went to m2v. Private buffers made and freed leave the pool's chunks
/// empty; shared buffers (write-combined on the facade) made until the device holds one write-combined chunk more,
/// then freed newest first, leave that chunk the device's empty spare, a member the kernel took out of its set at the
/// buffers' CPU leases; the bench's raw device opens and closes in this process on a connection of its own; nothing
/// completes for longer than the idle give-back's second (GpuMemoryRelease.h), which frees the empty chunks; then a
/// buffer of a chunk of its own makes the set again and command buffers fill and copy it. Every command buffer stays
/// raw with the right bytes and the kernel refuses no registration of the set (set_refusals; a refusal names its
/// member).
struct SetSurvivesTrimPathTest: GPUPathTest {
    let id = "gpu.path.set_survives_trim"
    let name = "The resource set outlives the idle give-back after a second raw device's life"
    let counters: [String] = []
    static let small = 4096, pooled = 64 * 1024, own = 1 << 20, rounds = 8, mostShared = 2048

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let before = RawFacade.counters(of: gpu.device)
        guard before["set_refusals"] != nil else { throw TestFailure("no resource set counters: a release runtime") }
        do {
            let held = (0..<8).compactMap { _ in
                gpu.device.makeBuffer(length: Self.pooled, options: .storageModePrivate)
            }
            metrics.set("private_buffers", held.count)
        }
        let wcBefore = RawFacade.memory(of: gpu.device)["wc"] ?? 0
        var shared: [MTLBuffer] = []
        while (RawFacade.memory(of: gpu.device)["wc"] ?? 0) <= wcBefore && shared.count < Self.mostShared {
            shared.append(try gpu.buffer(Self.small))
        }
        let wcPeak = RawFacade.memory(of: gpu.device)["wc"] ?? 0
        let made = shared.count
        metrics.set("shared_buffers", made)
        while !shared.isEmpty { shared.removeLast() }
        _ = try BenchRunner.open()
        try await Task.sleep(nanoseconds: 2_000_000_000)
        let trimmed = RawFacade.memory(of: gpu.device)
        metrics.set("wc_peak_kb", wcPeak >> 10)
        metrics.set("wc_after_kb", (trimmed["wc"] ?? 0) >> 10)
        metrics.set("pool_after_kb", (trimmed["pool"] ?? 0) >> 10)
        var failures: [String] = []
        if wcPeak <= wcBefore {
            failures.append("\(made) shared buffers made no write-combined chunk")
        } else if (trimmed["wc"] ?? 0) >= wcPeak {
            failures.append("the idle give-back freed no write-combined chunk")
        }
        let target = try gpu.buffer(Self.own), readback = try gpu.buffer(Self.small)
        var wrong = 0
        for round in 0..<Self.rounds {
            let value = UInt8(0x30 + round)
            try await gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.fill(buffer: target, range: 0..<Self.own, value: value)
                blit.copy(
                    from: target, sourceOffset: Self.own - Self.small, to: readback, destinationOffset: 0,
                    size: Self.small)
                blit.endEncoding()
            }
            wrong += UnsafeRawBufferPointer(start: readback.contents(), count: Self.small).filter { $0 != value }.count
        }
        let after = RawFacade.counters(of: gpu.device)
        let refusals = (after["set_refusals"] ?? 0) - (before["set_refusals"] ?? 0)
        metrics.set("set_refusals", refusals)
        metrics.set("set_rebuilds", (after["set_rebuilds"] ?? 0) - (before["set_rebuilds"] ?? 0))
        metrics.set("wrong_bytes", wrong)
        if refusals != 0 {
            failures.append("the kernel refused \(refusals) registrations: \(RawFacade.setRefusal(of: gpu.device))")
        }
        if wrong != 0 { failures.append("\(wrong) bytes read back wrong") }
        return failures
    }
}
