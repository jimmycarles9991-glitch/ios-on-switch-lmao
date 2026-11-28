import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import Metal

/// GPURates.fill on the default device (gpu/GPURatesFill.swift): metal.benchmark's fill across target, memory mapping
/// and band budget, to name what halved it between the 24 and 25 Nov builds. It fails where a cell threw or its probe
/// did not reach the target's BO, and on the facade where the probe is missing. The rates are for comparing cells, not
/// judged here; the log line keeps the rates and the proof fields.
struct GPUFillTest: HWTest {
    let id = "gpu.perf.fill"
    let name = "metal.benchmark's fill by target, SMMU big pages or direct addresses, and band budget"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let production = DrawBandsTest.budget(gpu.device, 0)
        defer { production.map { _ = DrawBandsTest.budget(gpu.device, $0) } }
        let result = try await GPURates.fill(
            gpu, productionBudget: production, settleCompression: { try await CompressionSettled.settle(gpu.device) }
        )
        var metrics = result.metrics
        metrics.set("dev", GM20BFacade.kind(of: gpu.device))
        let kept = metrics.keys.filter {
            $0.hasSuffix("_gpix_s") || $0.hasSuffix("_error") || $0.hasPrefix("lane_mhz") || $0.hasSuffix("_bos_smmu")
        }
        metrics.fitLog(within: 900, keeping: Set(kept))
        var failures = result.passed ? [] : result.reasons(limit: 4)
        if GM20BFacade.kind(of: gpu.device) == "gm20b" { failures += result.notes }
        if !failures.isEmpty { return .fail(failures.joined(separator: "; "), metrics) }
        return .pass(metrics, proof: gpu.proof).onRawPath(gpu)
    }
}
