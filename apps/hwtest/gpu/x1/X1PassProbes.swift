import Foundation
import HWTestCore
import Metal

/// METALMAP d17: visibility results. A 100 x 100 quad drawn twice under Counting at offset 0 and once under Boolean at
/// offset 8 in one pass, then once under Counting at offset 0 in a second pass. Metal adds the samples of every draw at
/// an offset within a pass (20,000), and a later pass replaces it (10,000 on an M-series Mac, X1ProbeTests).
public enum X1PassProbes {
    public static func visibility(_ gpu: GPU) async throws -> ProbeFinding {
        let size = 128
        let target = try gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget, .shaderRead])
        let results = try gpu.buffer(16)
        memset(results.contents(), 0, 16)
        let state = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let quad = GPU.pixelQuad(0, 0, 100, 100, width: size, height: size)
        var reads: [UInt64] = []
        var paths: [String] = []
        for passIndex in 0..<2 {
            paths.append(
                try await gpu.run { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = target
                    pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].storeAction = .store
                    pass.visibilityResultBuffer = results
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(state)
                    encoder.setVisibilityResultMode(.counting, offset: 0)
                    GPU.draw(encoder, corners: quad, color: SIMD4(1, 0, 0, 1))
                    if passIndex == 0 {
                        GPU.draw(encoder, corners: quad, color: SIMD4(0, 1, 0, 1))
                        encoder.setVisibilityResultMode(.boolean, offset: 8)
                        GPU.draw(encoder, corners: quad, color: SIMD4(0, 0, 1, 1))
                    }
                    encoder.endEncoding()
                }.path)
            reads.append(results.contents().load(fromByteOffset: 0, as: UInt64.self))
            if passIndex == 0 { reads.append(results.contents().load(fromByteOffset: 8, as: UInt64.self)) }
        }
        var metrics = Metrics()
        metrics.set("pass1_count", Int(reads[0]))
        metrics.set("pass1_boolean", Int(reads[1]))
        metrics.set("pass2_count", Int(reads[2]))
        let wrong = [
            "pass1_count_wrong": reads[0] == 20_000 ? 0 : 1, "boolean_wrong": reads[1] != 0 ? 0 : 1,
            "pass2_count_wrong": reads[2] == 10_000 ? 0 : 1,
        ]
        return ProbeFinding.counted(wrong, paths: paths, metrics: metrics)
    }
}
