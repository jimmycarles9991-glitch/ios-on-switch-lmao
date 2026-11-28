import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// GPURates.firstTouch on the default device with the facade's band budget at 0 (gpu/GPURatesFirstTouch.swift): the
/// arms whose state no device run has pushed before. Registry.firstTouch runs it after every other test of both apps.
/// It fails where an arm threw, drew wrong pixels with its probe off, or its probe moved nothing, and on the facade
/// where a probe is missing.
struct GPUFirstTouchTest: HWTest {
    let id = "gpu.perf.first_touch"
    let name = "First pushes on the chip: subtiling knob 0x06164010, zcull Z_4X2 and render-target discard, A/B"
    let subsystem = Subsystem.metal

    static let judged = ["zcull_4x2_off_wrong"]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let production = DrawBandsTest.budget(gpu.device, 0)
        defer { production.map { _ = DrawBandsTest.budget(gpu.device, $0) } }
        let result = try await GPURates.firstTouch(gpu)
        var metrics = result.metrics
        metrics.set("dev", GM20BFacade.kind(of: gpu.device))
        var failures = result.passed ? [] : result.reasons(limit: 4)
        for key in Self.judged {
            if case .integer(let wrong) = metrics[key], wrong > 0 { failures.append("\(key)=\(wrong)") }
        }
        if GM20BFacade.kind(of: gpu.device) == "gm20b" { failures += result.notes }
        let kept = metrics.keys.filter {
            $0.hasSuffix("_error") || $0.hasSuffix("_wrong") || $0.hasSuffix("_writes_per_pass")
                || $0.hasSuffix("_kept_px")
        }
        metrics.fitLog(within: 900, keeping: Set(Self.judged + kept))
        if !failures.isEmpty { return .fail(failures.joined(separator: "; "), metrics) }
        return .pass(metrics, proof: gpu.proof).onRawPath(gpu)
    }
}
