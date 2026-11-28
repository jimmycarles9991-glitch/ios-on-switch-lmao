import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.gm20b.zoom_target's passes over Core Animation's zoom window target (1792x1472 BGRA8), and its redrawn arm's: a
/// pass that loads the target and draws a solid rectangle over part of what it held, and the texture unit's samples of
/// it at `points` through its header, so a header still naming memory the target left reads the old texels there.
public enum ZoomTargetScene {
    public static let width = 1792, height = 1472
    /// Two pixels inside the redrawn rectangle and two the passes leave cleared, and what an M4 Max samples there after
    /// the redraw
    public static let points: [SIMD2<UInt32>] = [[700, 700], [1000, 900], [100, 100], [1700, 1400]]
    public static let redrawnCodes: [[UInt8]] = [[64, 191, 128, 255], [64, 191, 128, 255], [0, 0, 0, 0], [0, 0, 0, 0]]
    static let redrawn: SIMD4<Float> = [0.25, 0.75, 0.5, 1]

    /// One pass as Core Animation's over the zoom target: its colour cleared and stored, the memoryless second colour
    /// cleared and not stored, one rectangle under a 520x250 scissor
    public static func draw(
        _ gpu: GPU, _ pipeline: MTLRenderPipelineState, _ target: MTLTexture, _ scratch: MTLTexture
    ) async throws
        -> String
    {
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[1].texture = scratch
            pass.colorAttachments[1].loadAction = .clear
            pass.colorAttachments[1].storeAction = .dontCare
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            CAState.bind(
                encoder,
                vertices: CAState.rectangle(
                    x0: -0.75, y0: -0.5, x1: 0.25, y1: 0.4, color: SIMD4<Float16>(0.75, 0.5, 0.25, 1)
                ),
                source: nil
            )
            encoder.setScissorRect(MTLScissorRect(x: 250, y: 130, width: 520, height: 250))
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }.path
    }

    /// A pass that loads the target and draws a solid rectangle over pixels (512, 512) to (1280, 1088)
    public static func redraw(_ gpu: GPU, _ target: MTLTexture) async throws -> String {
        let solid = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        return try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(solid)
            let x0 = Float(512) / Float(width) * 2 - 1, x1 = Float(1280) / Float(width) * 2 - 1
            let y0 = 1 - Float(1088) / Float(height) * 2, y1 = 1 - Float(512) / Float(height) * 2
            GPU.draw(encoder, corners: GPU.quad(x0: x0, y0: y0, x1: x1, y1: y1, z: 0.5), color: redrawn)
            encoder.endEncoding()
        }.path
    }

    /// The target's texels at `points` as the texture unit samples them in a kernel, RGBA8 codes, and the path
    public static func sampled(_ gpu: GPU, _ target: MTLTexture) async throws -> ([[UInt8]], String) {
        let kernel = try gpu.compute("sampled_points")
        let at = try gpu.buffer(points)
        let out = try gpu.buffer(points.count * MemoryLayout<SIMD4<Float>>.stride)
        let path = try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(kernel)
            encoder.setTexture(target, index: 0)
            encoder.setBuffer(at, offset: 0, index: 0)
            encoder.setBuffer(out, offset: 0, index: 1)
            encoder.dispatchThreads(
                MTLSize(width: points.count, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: points.count, height: 1, depth: 1)
            )
            encoder.endEncoding()
        }.path
        let texels = UnsafeBufferPointer(
            start: out.contents().bindMemory(to: SIMD4<Float>.self, capacity: points.count), count: points.count
        )
        return (texels.map { texel in (0 ..< 4).map { UInt8(max(0, min(255, (texel[$0] * 255).rounded()))) } }, path)
    }
}
