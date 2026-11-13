import Foundation
import HWTestCore

/// The display's window probes (drivers/NativeMobileDisplay/DisplayWindowProbe.h, debug kernels): each shows test
/// pictures on the panel for about 0.7 s, through a route the display does not use yet and through a known-good one,
/// and DC A's CRC of the two frames says whether the route scans the same picture. The kernel holds the swaps, runs
/// the frames and restores windows A and B by itself; the test starts it and reads the result. Run after the other
/// display tests.
struct DisplayWindowProbeTest: HWTest {
    let id: String
    let name: String
    let probe: String
    let number: UInt32
    let subsystem = Subsystem.display
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        ProbeJournal(probe: id).starting("display window probe \(probe)")
        let finding: ProbeFinding
        switch await Self.result(number) {
        case .failure(let failure): finding = ProbeFinding(.inconclusive, note: failure.description)
        case .success(let reply): finding = reply.finding
        }
        return finding.outcome(probe: probe, base: Metrics(), proof: "")
    }

    /// Starts the probe, then reads the result every 100 ms until the run is done (it takes about a second).
    static func result(_ number: UInt32) async -> Result<DisplayWindowProbeReply, TestFailure> {
        let started: DisplayWindowProbeReply
        switch exchange(DisplayWindowProbeReply.start, number) {
        case .failure(let failure): return .failure(failure)
        case .success(let reply): started = reply
        }
        guard started.error == 0 else {
            return .failure(TestFailure("not started (errno \(started.error)): \(started.why)"))
        }
        for _ in 0..<100 {
            try? await Task.sleep(nanoseconds: 100_000_000)
            switch exchange(DisplayWindowProbeReply.query, number) {
            case .failure(let failure): return .failure(failure)
            case .success(let reply):
                if reply.probe == number && reply.state == DisplayWindowProbeReply.done { return .success(reply) }
            }
        }
        return .failure(TestFailure("the run did not finish in 10 s"))
    }

    static func exchange(_ op: UInt32, _ number: UInt32) -> Result<DisplayWindowProbeReply, TestFailure> {
        let request = DisplayWindowProbeReply.request(op: op, probe: number)
        return VICKernelTest.call(request, replyBytes: DisplayWindowProbeReply.size).flatMap { bytes in
            DisplayWindowProbeReply(bytes).map { .success($0) } ?? .failure(TestFailure("malformed reply"))
        }
    }

    static let all: [any HWTest] = [
        DisplayWindowProbeTest(
            id: "display.x1.overlay_block_yuv", name: "NV12 in 2- and 16-GOB blocks on window B against pitch (CRC)",
            probe: "V5a", number: 1),
        DisplayWindowProbeTest(
            id: "display.x1.column_scan_yuv",
            name: "Window A column-scanning NVDEC's landscape NV12 both ways, and the UI's BGRA on window B (CRC)",
            probe: "V3",
            number: 2),
        DisplayWindowProbeTest(
            id: "display.x1.offset_crop", name: "A block-linear NV12 crop through window B's H/V offsets (CRC)",
            probe: "V5c", number: 3),
        DisplayWindowProbeTest(
            id: "display.x1.window_formats",
            name: "NV12 Cb first at depth 43, NV24 at 49, packed 4:2:2 at 16 and SWAP2, against BGRA (CRC)",
            probe: "L979/L955", number: 4),
        DisplayWindowProbeTest(
            id: "display.x1.window_wide", name: "Window B's V filter on 3840- and 4096-wide lines (CRC)",
            probe: "L958", number: 5),
        DisplayWindowProbeTest(
            id: "display.x1.window_palette", name: "Windows A and B with CP_ENABLE and a per-channel palette (CRC)",
            probe: "L1004", number: 6),
    ]
}
