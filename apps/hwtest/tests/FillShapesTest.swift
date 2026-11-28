import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// fillTexture: (SPI) shapes that went to m2v, each now raw's (FillShapeCases.swift, every case's bytes as an M4 Max
/// fills them): an RGB9E5 region, planes 1 to 3 of a 3D texture's region, every sample of a 4x texture (through a
/// resolve), Depth32Float_Stencil8's depth through R32Float keeping its stencil, RGBA32Float through R8Unorm, RGBA8
/// through RGBA16Float (the half texel cut to 4 bytes) and a cube array's slice 7. Per case: <case>_path (raw),
/// <case>_inside_wrong and <case>_outside_changed (0 each), and no command buffer failed over the test (failed_cbs=).
struct FillShapesTest: HWTest {
    let id = "gpu.gm20b.fill_shapes"
    let name = "fillTexture: into RGB9E5, 3D, multisampled, depth, cube-array and narrower-format texels, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for (key, run) in FillShapeCases.all {
            let ran: FillShapeResult?
            do {
                ran = try await run(gpu)
            } catch {
                checks.append(("\(key): \(GPU.describe(error))", false))
                continue
            }
            guard let result = ran else { return .skip("the blit encoder answers no fillTexture:") }
            metrics.set("\(key)_path", String(result.path.prefix(80)))
            metrics.set("\(key)_inside_wrong", result.insideWrong)
            metrics.set("\(key)_outside_changed", result.outsideChanged)
            checks.append(("\(key): on the driver", result.path == "gm20b"))
            checks.append(
                (
                    "\(key): the region at Metal's texel, the rest as it was",
                    result.insideWrong + result.outsideChanged == 0
                )
            )
        }
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: ["failed_cbs"])
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
