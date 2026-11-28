import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// GPURates.knobs on the default device with the facade's band budget at 0: the subtiling knob and texture sector
/// promotion A/B (gpu/GPURatesKnobs.swift). It passes once every arm ran with GPU timestamps, none threw, and each
/// probe showed it fired (the knob written, the promotion in the header); on the facade both probes must be there. The
/// numbers are for comparing runs, not judged here. The log line keeps the proof fields and any arm's error; the report
/// keeps every field.
struct GPUKnobsTest: HWTest {
    let id = "gpu.perf.knobs"
    let name = "Subtiling perf knob and texture sector promotion, A/B per draw shape"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let production = DrawBandsTest.budget(gpu.device, 0)
        defer { production.map { _ = DrawBandsTest.budget(gpu.device, $0) } }
        let result = try await GPURates.knobs(gpu)
        var metrics = result.metrics
        metrics.set("dev", GM20BFacade.kind(of: gpu.device))
        let proofs = metrics.keys.filter {
            $0.hasSuffix("_error") || $0.hasSuffix("_writes_per_pass") || $0.hasSuffix("_header")
        }
        metrics.fitLog(within: 900, keeping: Set(proofs + ["lane_mhz"]))
        if !result.passed { return .fail(result.reasons(limit: 4).joined(separator: "; "), metrics) }
        if GM20BFacade.kind(of: gpu.device) == "gm20b" && !result.notes.isEmpty {
            return .fail(result.notes.joined(separator: "; "), metrics)
        }
        return .pass(metrics, proof: gpu.proof).onRawPath(gpu)
    }
}
