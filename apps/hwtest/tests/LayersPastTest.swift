import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// renderTargetArrayLength past some attachments' layers (LayersPastCases.mixed): each attachment keeps the layers it
/// has, and past a cleared depth's layers the test is against the clear value. Expected values are an M4 Max's
/// (bitsprobe.mm layersMixed, this scene measured): each layer's halves, left then right, k the clear, d the layer's own
/// colour, w the right half's depth 0.125: layers_past_c0_l0..l3 kd (nothing passes 0.5 Less against the clear 0.25,
/// past the depth's layers too, and everything passes 0.125), layers_past_c1_l0..l1 kd, layers_past_depth_l0..l1 kw.
/// The driver renders the shorter attachments through backings of the pass's layers (pass_begin.cpp shortOf).
struct LayersPastTest: HWTest {
    let id = "gpu.gm20b.layers_past"
    let name =
        "A layered pass past some attachments' layers keeps each one's layers and tests against the clear, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        return await Self.outcome(gpu)
    }

    static func outcome(_ gpu: GPU) async -> Outcome {
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            let r = try await LayersPastCases.mixed(gpu)
            let codes: [(String, [String])] = [
                ("c0", r.c0.enumerated().map { LayersPastCases.halves($1, layer: $0) }),
                ("c1", r.c1.enumerated().map { LayersPastCases.halves($1, layer: $0) }),
                ("depth", r.depth.map { LayersPastCases.halves($0, kept: 0.25, written: 0.125) }),
            ]
            for (target, layers) in codes {
                let want = target == "depth" ? "kw" : "kd"
                for (l, code) in layers.enumerated() {
                    metrics.set("layers_past_\(target)_l\(l)", code)
                    checks.append(("\(target) layer \(l): \(want)", code == want))
                }
            }
        } catch {
            checks.append(("the pass: \(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
