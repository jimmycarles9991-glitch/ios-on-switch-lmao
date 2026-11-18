import Foundation
import HWTestCore
import Metal

/// Depth bias on a Depth32Float target, read back exactly: no bias leaves z alone, a constant bias moves a flat quad
/// by the same amount everywhere (bias x 2^(e - 23) for the largest z's exponent e, as Vulkan and Apple's GPUs have it), the opposite bias by the opposite amount, twice the bias by twice as much, a clamp
/// caps it, and a slope-scaled bias moves a sloped quad by the scale times its slope.
struct DepthBiasScene: GPUScene {
    let id = "gpu.draw.depth_bias"
    let name = "Depth bias: constant, slope-scaled and clamped"

    static let size = 64

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm, depth: .depth32Float)
        let always = MTLDepthStencilDescriptor()
        always.depthCompareFunction = .always
        always.isDepthWriteEnabled = true
        guard let state = gpu.device.makeDepthStencilState(descriptor: always) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        let flat = GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.5)
        let sloped = [
            SIMD4<Float>(-1, -1, 0.25, 1), SIMD4(1, -1, 0.75, 1), SIMD4(-1, 1, 0.25, 1), SIMD4(1, 1, 0.75, 1),
        ]
        func depths(_ corners: [SIMD4<Float>], bias: Float, slope: Float, clamp: Float) async throws -> [Float] {
            let color = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
            let depth = try gpu.texture(.depth32Float, Self.size, Self.size, usage: [.renderTarget, .shaderRead])
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = color
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .dontCare
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = 1
                pass.depthAttachment.storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                encoder.setDepthStencilState(state)
                encoder.setDepthBias(bias, slopeScale: slope, clamp: clamp)
                GPU.draw(encoder, corners: corners, color: SIMD4(1, 1, 1, 1))
                encoder.endEncoding()
            }
            return try await gpu.raster(depth, .depth32Float).samples
        }
        let unbiased = try await depths(flat, bias: 0, slope: 0, clamp: 0)
        result.expect(unbiased.allSatisfy { $0 == 0.5 }, "no bias: depth \(unbiased[0]) != 0.5")
        let up = try await depths(flat, bias: 4096, slope: 0, clamp: 0).map { $0 - 0.5 }
        let down = try await depths(flat, bias: -4096, slope: 0, clamp: 0).map { 0.5 - $0 }
        let double = try await depths(flat, bias: 8192, slope: 0, clamp: 0).map { $0 - 0.5 }
        let offset = up[0]
        let clamped = try await depths(flat, bias: 4096, slope: 0, clamp: offset / 4).map { $0 - 0.5 }
        result.metrics.set("bias_4096", Double(offset), digits: 9)
        result.expect(offset == 4096 * pow(2, -24), "bias 4096 moved z 0.5 by \(offset), not 4096 x 2^-24 (r for 0.5)")
        result.expect(up.allSatisfy { $0 == offset }, "bias 4096 moved a flat quad unevenly")
        result.expect(down.allSatisfy { abs($0 - offset) <= offset * 0.001 }, "bias -4096 moved z by \(down[0])")
        result.expect(double.allSatisfy { abs($0 - 2 * offset) <= offset * 0.01 }, "bias 8192 moved z by \(double[0])")
        result.expect(
            clamped.allSatisfy { abs($0 - offset / 4) <= offset * 0.01 }, "clamp \(offset / 4) let z move \(clamped[0])"
        )

        let slope = Float(0.5) / Float(Self.size)
        let plain = try await depths(sloped, bias: 0, slope: 0, clamp: 0)
        let scaled = try await depths(sloped, bias: 0, slope: 2, clamp: 0)
        let moved = zip(scaled, plain).map { $0 - $1 }
        result.metrics.set("slope_bias", Double(moved[moved.count / 2]), digits: 6)
        result.expect(
            moved.allSatisfy { abs($0 - 2 * slope) <= 2 * slope * 0.05 },
            "slope scale 2 on slope \(slope) moved z by \(moved.min() ?? 0)...\(moved.max() ?? 0), not \(2 * slope)")
        return result
    }
}
