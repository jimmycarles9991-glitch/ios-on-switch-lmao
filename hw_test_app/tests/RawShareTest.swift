import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Where this process's command buffers went since it started, from the raw facade's own counters
/// (switchRawCounters): raw, m2v whole or split, and the predicates that sent work to m2v. It runs after every other
/// GPU test, so it counts theirs and the frameworks' (Core Animation, Core Image, MPS).
struct RawShareTest: HWTest {
    let id = "gpu.raw_share"
    let name = "Command buffers on raw and on m2v, and why"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        guard let device = MTLCreateSystemDefaultDevice(), RawFacade.kind(of: device) == "raw" else {
            return .skip("the default device is not the raw facade")
        }
        let counters = RawFacade.counters(of: device)
        guard !counters.isEmpty else { return .fail("the raw facade answered no counters (switchRawCounters)") }
        var metrics = PathTally.counterMetrics(before: [:], after: counters)
        let raw = counters["raw"] ?? 0
        let m2v = counters["m2v"] ?? 0
        let split = counters["split"] ?? 0
        let total = raw + m2v + split
        metrics.set("m2v_pct", total > 0 ? Double(m2v + split) * 100 / Double(total) : 0, digits: 1)
        return .judged([("command buffers were counted", total > 0)], metrics, proof: device.name)
    }
}
