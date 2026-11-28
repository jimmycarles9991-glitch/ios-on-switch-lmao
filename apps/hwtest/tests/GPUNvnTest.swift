import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import Metal

/// GPURates.nvn on the default device with the facade's band budget at 0 (gpu/GPURatesNvn.swift): NVN's zcull, discard,
/// 4x compression kind and tiled-cache flush choices against ours. It fails where an arm drew wrong pixels with its probe
/// off, where an arm threw, where an on arm's probe moved nothing, and on the facade where a probe is missing. A probe's
/// own wrong pixels are its verdict, reported, not judged. The log line keeps the judged fields, the on arms' wrong
/// pixels and any arm's error; the report keeps every field.
struct GPUNvnTest: HWTest {
    let id = "gpu.perf.nvn"
    let name = "NVN's zcull, discard, 4x compression kind and tiled-flush choices, A/B"
    let subsystem = Subsystem.metal

    /// The arms whose wrong pixels with the probe off are the driver's own fault
    static let judged = [
        "zcull_bounds_hidden_off_wrong", "stencil_zcull_off_wrong", "zcull_fp_off_wrong",
        "zcull_always_off_culled_px", "zcull_not_equal_off_culled_px", "zcull_rule_control_off_culled_px",
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let production = DrawBandsTest.budget(gpu.device, 0)
        defer { production.map { _ = DrawBandsTest.budget(gpu.device, $0) } }
        let result = try await GPURates.nvn(gpu, settleCompression: { try await CompressionSettled.settle(gpu.device) })
        var metrics = result.metrics
        metrics.set("dev", GM20BFacade.kind(of: gpu.device))
        var failures = result.passed ? [] : result.reasons(limit: 4)
        for key in Self.judged {
            if case .integer(let wrong) = metrics[key], wrong > 0 { failures.append("\(key)=\(wrong)") }
        }
        if GM20BFacade.kind(of: gpu.device) == "gm20b" { failures += result.notes }
        let kept = metrics.keys.filter { $0.hasSuffix("_error") || $0.hasSuffix("_on_wrong") }
        metrics.fitLog(within: 900, keeping: Set(Self.judged + kept))
        if !failures.isEmpty { return .fail(failures.joined(separator: "; "), metrics) }
        return .pass(metrics, proof: gpu.proof).onRawPath(gpu)
    }
}
