import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// GPURates on the default device with the facade's band budget at 0, so no arm is cut into bands or pieces: the
/// rates are the units', each the fastest of its runs. It passes once every arm ran on the driver with GPU timestamps;
/// the numbers are for comparing runs (//tools/hwtest), not judged here.
struct GPURatesTest: HWTest {
    let id = "gpu.perf.rates"
    let name = "GPU unit rates: FMA, bandwidth, fill, texels, triangles, hidden layers, shared buffers cached and not"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let production = DrawBandsTest.budget(gpu.device, 0)
        defer { production.map { _ = DrawBandsTest.budget(gpu.device, $0) } }
        let result = try await GPURates.run(gpu)
        var metrics = result.metrics
        metrics.set("dev", GM20BFacade.kind(of: gpu.device))
        metrics.set("bands_off", production != nil)
        if !result.passed { return .fail(result.reasons(limit: 4).joined(separator: "; "), metrics) }
        if GM20BFacade.kind(of: gpu.device) == "gm20b" && !result.notes.isEmpty {
            return .fail(result.notes.joined(separator: "; "), metrics)
        }
        return .pass(metrics, proof: gpu.proof).onRawPath(gpu)
    }
}
