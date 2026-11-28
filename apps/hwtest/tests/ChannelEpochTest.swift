import Foundation
import HWTestCore
import HWTestKit
import SwitchBench

/// The GPU's channel epoch (TegraGM20BABI.h TegraGM20BChannelEpochWord, drivers/TegraGM20B/gpu_chan_epoch.c) on two
/// TegraGM20B connections of the test's own, A and B (bench/bench_channel_epoch.cpp; the app's Metal device and its
/// channel are not touched): A's copy-only ChannelCreates fill its 8 fence slots and the ninth is refused
/// kIOReturnNoResources; A destroying one moves the epoch in A's fence page and in B's before the destroy returns, one
/// read each with no wait (epoch_moved, other_page_moved); the next ChannelCreate succeeds at once and a copy-engine
/// fill job on that channel lands and is waited for (recreate_kr, job_kr, wait_kr 0, job_filled 1). The expected values
/// are the kernel's contract, which no Apple GPU has. Reported: fence_token, epoch_before, moves_while_full (other
/// processes' destroys meanwhile), destroy_to_create_ns. Every channel, BO and both connections go at the end, which
/// moves the epoch for every process once more: a client asks again only where it was refused.
struct ChannelEpochTest: HWTest {
    let id = "gpu.kernel.channel_epoch"
    let name = "A refused channel's freed slot moves the channel epoch at once, and the next channel and job succeed"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    static let noResources = Int32(bitPattern: 0xe000_02be)

    func run(_ context: TestContext) async throws -> Outcome {
        var r = switch_bench_channel_epoch()
        switch_bench_channel_epoch_run(&r)
        let moved = Int32(bitPattern: r.epoch_after &- r.epoch_refused)
        let other = Int32(bitPattern: r.epoch_other &- r.epoch_refused)
        var metrics = Metrics()
        metrics.set("epoch_flag", Int(r.epoch_flag))
        metrics.set("fence_token", "0x" + String(r.fence_token, radix: 16))
        metrics.set("slots_made", Int(r.slots_made))
        metrics.set("refused_kr", Self.hex(r.refused_kr))
        metrics.set("epoch_before", Int(r.epoch_before))
        metrics.set("moves_while_full", Int(Int32(bitPattern: r.epoch_refused &- r.epoch_before)))
        metrics.set("epoch_moved", Int(moved))
        metrics.set("other_page_moved", other >= 1)
        metrics.set("recreate_kr", Self.hex(r.recreate_kr))
        metrics.set("destroy_to_create_ns", Int(r.destroy_to_create_ns))
        metrics.set("job_kr", Self.hex(r.job_kr))
        metrics.set("wait_kr", Self.hex(r.wait_kr))
        metrics.set("job_filled", Int(r.job_filled))
        let why = withUnsafeBytes(of: &r.why) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        if !why.isEmpty { metrics.set("stopped", why) }
        let reached = r.recreate_kr != -1
        return .judged(
            [
                ("epoch_flag=\(r.epoch_flag) not 1", r.epoch_flag == 1),
                ("slots_made=\(r.slots_made) not 8", r.slots_made == 8),
                ("refused_kr=\(Self.hex(r.refused_kr)) not kIOReturnNoResources", r.refused_kr == Self.noResources),
                ("epoch_moved=\(moved) not >= 1", reached && moved >= 1),
                ("other_page_moved: \(other) not >= 1", reached && other >= 1),
                ("recreate_kr=\(Self.hex(r.recreate_kr)) not 0", r.recreate_kr == 0),
                ("job_kr=\(Self.hex(r.job_kr)) not 0", r.job_kr == 0),
                ("wait_kr=\(Self.hex(r.wait_kr)) not 0", r.wait_kr == 0),
                ("job_filled=\(r.job_filled) not 1", r.job_filled == 1),
            ], metrics, detail: why
        )
    }

    static func hex(_ kr: Int32) -> String { kr == -1 ? "unreached" : "0x" + String(UInt32(bitPattern: kr), radix: 16) }
}
