import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestScenes
import Metal

/// GPURates.caBinding on the default device under production's band budget, as the render server draws
/// (gpu/GPURatesCaBinding.swift): Core Animation's costliest states fed from Shared buffers bound L2-cached and
/// L2-bypassing, the data for the render server's binding. It fails where a replay threw, drew off the driver, or its
/// ring bound otherwise than its arm says, and on the facade where the probe is missing. The times are not judged.
struct GPUCaBindingTest: HWTest {
    let id = "gpu.perf.ca_binding"
    let name = "Core Animation's hot states fed from Shared buffers, L2-cached against L2-bypassing"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let result = try await GPURates.caBinding(gpu, states: CAHotCorpus.states)
        var metrics = result.metrics
        metrics.set("dev", GM20BFacade.kind(of: gpu.device))
        let kept = metrics.keys.filter { $0.hasSuffix("_error") || $0.hasSuffix("_stall_us") }
        metrics.fitLog(within: 900, keeping: Set(kept))
        var failures = result.passed ? [] : result.reasons(limit: 4)
        if GM20BFacade.kind(of: gpu.device) == "gm20b" { failures += result.notes }
        if !failures.isEmpty { return .fail(failures.joined(separator: "; "), metrics) }
        return .pass(metrics, proof: gpu.proof).onRawPath(gpu)
    }
}
