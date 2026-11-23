import Foundation
import HWTestCore
import Metal

/// Dispatches of more threadgroups down or deep than one QMD's grid holds (65535): raw launches them in slices based
/// where each starts (dispatch_bands.h kQmdGroups), where it once refused them as compute.nokernel. Each
/// threadgroup writes its index plus one at its index, so a threadgroup run twice, never or in the wrong place shows.
struct GridSlicesScene: GPUScene {
    let id = "gpu.gm20b.grid_slices"
    let name = "Dispatches past the 65535 threadgroups down or deep one launch holds"

    static let grids = [MTLSize(width: 2, height: 70000, depth: 1), MTLSize(width: 1, height: 3, depth: 70001)]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let pipeline = try gpu.compute("grid_positions")
        var paths: [String] = []
        for (n, grid) in Self.grids.enumerated() {
            let count = grid.width * grid.height * grid.depth
            let out = try gpu.buffer(count * 4)
            memset(out.contents(), 0, count * 4)
            let timing = try await gpu.run { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(pipeline)
                encoder.setBuffer(out, offset: 0, index: 0)
                encoder.dispatchThreadgroups(grid, threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
                encoder.endEncoding()
            }
            paths.append(timing.path)
            let words = out.contents().assumingMemoryBound(to: UInt32.self)
            var wrong = 0
            var first = -1
            for i in 0 ..< count where words[i] != UInt32(i + 1) {
                wrong += 1
                if first < 0 { first = i }
            }
            result.metrics.set("grid\(n)", "\(grid.width)x\(grid.height)x\(grid.depth)")
            result.metrics.set("grid\(n)_wrong", wrong)
            result.expect(wrong == 0, "grid \(n): \(wrong) of \(count) threadgroups wrong, the first at \(first)")
        }
        result.metrics.set("paths", paths.joined(separator: ","))
        if GM20BFacade.kind(of: gpu.device) == "gm20b" {
            result.expect(paths.allSatisfy { $0 == "gm20b" }, "the dispatches took \(paths), not raw")
        }
        return result
    }
}
