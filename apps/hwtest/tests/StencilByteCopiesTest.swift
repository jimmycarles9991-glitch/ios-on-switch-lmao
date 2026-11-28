import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// X32_Stencil8 copied into R8Uint and back with the blit encoder (StencilByteCopies.swift): out_wrong= R8Uint bytes
/// that are not the stencil, in_wrong= stencil bytes that are not R8Uint's, depth_changed= depths the copy in moved,
/// paths=, failed_cbs=. An M4 Max: 0, 0, 0.
struct StencilByteCopiesTest: HWTest {
    let id = "gpu.gm20b.stencil_byte_copies"
    let name = "Blit copies between an X32_Stencil8 view and R8Uint move the stencil bytes as they are, on raw"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        let result = try await StencilByteCopies.run(gpu)
        var metrics = Metrics()
        metrics.set("out_wrong", result.outWrong)
        metrics.set("in_wrong", result.inWrong)
        metrics.set("depth_changed", result.depthChanged)
        metrics.set("paths", result.paths.map { String($0.prefix(60)) }.joined(separator: ","))
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        return .judged(
            [
                ("X32_Stencil8 into R8Uint keeps every byte", result.outWrong == 0),
                ("R8Uint into X32_Stencil8 keeps every byte", result.inWrong == 0),
                ("the copy in keeps the depth", result.depthChanged == 0),
                ("both copies on the driver", result.paths.allSatisfy { $0 == "gm20b" }),
                FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics),
            ], metrics, proof: gpu.proof
        )
    }
}
