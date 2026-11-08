import Foundation
import HWTestCore
import HWTestGPU
import Metal

struct MetalColorPassTest: HWTest {
    let id = "metal.render.color"
    let name = "Render pass: colour, exact readback"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let size = 64
        let target = try gpu.texture(.rgba8Unorm, size, size)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 1, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            // Left half: pixel-aligned edges, no partial coverage.
            GPU.draw(encoder, corners: GPU.quad(x0: -1, y0: -1, x1: 0, y1: 1, z: 0.5), color: SIMD4(0.2, 0.4, 0.6, 1))
            encoder.endEncoding()
        }
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        var wrong = 0
        for y in 0..<size {
            for x in 0..<size
            where Array(pixels.pixel(x, y, width: size)) != (x < size / 2 ? [51, 102, 153, 255] : [0, 0, 255, 255]) {
                wrong += 1
            }
        }
        var metrics = Metrics()
        metrics.set("wrong_pixels", wrong)
        metrics.set("gpu_ms", timing.gpuMilliseconds, digits: 3)
        return .judged(
            [("every pixel exact (corner reads \(Array(pixels.pixel(0, 0, width: size))))", wrong == 0)], metrics,
            proof: gpu.proof)
    }
}

/// Left half: a red quad at z 0.5 that writes stencil 1. Then green everywhere at z 0.7 (loses the depth test on the
/// left), then blue at z 0.1 where stencil is 1 without writing depth. Colour reads blue | green, depth 0.5 | 0.7,
/// stencil 1 | 0.
struct MetalDepthStencilTest: HWTest {
    let id = "metal.render.depth_stencil"
    let name = "Render pass: depth and stencil, exact readback"
    let subsystem = Subsystem.metal

    func depthState(
        _ device: MTLDevice, write: Bool, stencil: MTLCompareFunction, replace: Bool
    ) -> MTLDepthStencilState {
        let descriptor = MTLDepthStencilDescriptor()
        descriptor.depthCompareFunction = .less
        descriptor.isDepthWriteEnabled = write
        let face = MTLStencilDescriptor()
        face.stencilCompareFunction = stencil
        face.depthStencilPassOperation = replace ? .replace : .keep
        descriptor.frontFaceStencil = face
        descriptor.backFaceStencil = face
        return device.makeDepthStencilState(descriptor: descriptor)!
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let size = 64
        let color = try gpu.texture(.rgba8Unorm, size, size)
        let depth = try gpu.texture(.depth32Float_stencil8, size, size, usage: [.renderTarget])
        let pipeline = try gpu.render(
            fragment: "solid_fragment", format: .rgba8Unorm, depth: .depth32Float_stencil8,
            stencil: .depth32Float_stencil8)
        let writeStencil = depthState(gpu.device, write: true, stencil: .always, replace: true)
        let depthOnly = depthState(gpu.device, write: true, stencil: .always, replace: false)
        let stencilEqual = depthState(gpu.device, write: false, stencil: .equal, replace: false)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = color
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = .store
            pass.stencilAttachment.texture = depth
            pass.stencilAttachment.loadAction = .clear
            pass.stencilAttachment.clearStencil = 0
            pass.stencilAttachment.storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setStencilReferenceValue(1)
            encoder.setDepthStencilState(writeStencil)
            GPU.draw(encoder, corners: GPU.quad(x0: -1, y0: -1, x1: 0, y1: 1, z: 0.5), color: SIMD4(1, 0, 0, 1))
            encoder.setDepthStencilState(depthOnly)
            GPU.draw(encoder, corners: GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.7), color: SIMD4(0, 1, 0, 1))
            encoder.setDepthStencilState(stencilEqual)
            GPU.draw(encoder, corners: GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.1), color: SIMD4(0, 0, 1, 1))
            encoder.endEncoding()
        }
        let pixels = try await gpu.read(color, bytesPerPixel: 4)
        let depths = try await gpu.read(depth, bytesPerPixel: 4, options: .depthFromDepthStencil)
        let stencils = try await gpu.read(depth, bytesPerPixel: 1, options: .stencilFromDepthStencil)
        var (wrongColor, wrongDepth, wrongStencil) = (0, 0, 0)
        for y in 0..<size {
            for x in 0..<size {
                let left = x < size / 2
                if Array(pixels.pixel(x, y, width: size)) != (left ? [0, 0, 255, 255] : [0, 255, 0, 255]) {
                    wrongColor += 1
                }
                if depths.loaded(as: Float.self, at: y * size + x) != (left ? 0.5 : 0.7) { wrongDepth += 1 }
                if stencils[y * size + x] != (left ? 1 : 0) { wrongStencil += 1 }
            }
        }
        var metrics = Metrics()
        metrics.set("wrong_color", wrongColor)
        metrics.set("wrong_depth", wrongDepth)
        metrics.set("wrong_stencil", wrongStencil)
        return .judged(
            [
                ("colour exact", wrongColor == 0), ("depth exact (0.5 | 0.7)", wrongDepth == 0),
                ("stencil exact (1 | 0)", wrongStencil == 0),
            ], metrics, proof: gpu.proof)
    }
}

struct MetalMSAATest: HWTest {
    let id = "metal.render.msaa"
    let name = "Render pass: 4x MSAA and resolve"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard gpu.device.supportsTextureSampleCount(4) else { return .fail("4x MSAA unsupported") }
        let size = 64
        let samples = try gpu.texture(.rgba8Unorm, size, size, usage: [.renderTarget], samples: 4)
        let resolved = try gpu.texture(.rgba8Unorm, size, size)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm, samples: 4)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = samples
            pass.colorAttachments[0].resolveTexture = resolved
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .multisampleResolve
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            var corners: [SIMD4<Float>] = [SIMD4(-1, -1, 0.5, 1), SIMD4(1, -1, 0.5, 1), SIMD4(-1, 1, 0.5, 1)]
            var white = SIMD4<Float>(1, 1, 1, 1)
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 3, index: 0)
            encoder.setFragmentBytes(&white, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            encoder.endEncoding()
        }
        let pixels = try await gpu.read(resolved, bytesPerPixel: 4)
        // The triangle is the bottom-left half; its hypotenuse runs through the centres of the diagonal pixels
        // (x == y), which resolve to a blend. Pixels further from it are fully white or black.
        var (inside, outside, edge, wrong) = (0, 0, 0, 0)
        for y in 0..<size {
            for x in 0..<size {
                let red = pixels.pixel(x, y, width: size).first!
                let distance = y - x
                if distance > 1 {
                    if red == 255 { inside += 1 } else { wrong += 1 }
                } else if distance < -1 {
                    if red == 0 { outside += 1 } else { wrong += 1 }
                } else if red > 0 && red < 255 {
                    edge += 1
                }
            }
        }
        var metrics = Metrics()
        metrics.set("blended_edge_pixels", edge)
        metrics.set("wrong", wrong)
        return .judged(
            [
                ("interior and exterior exact", wrong == 0 && inside > 0 && outside > 0),
                ("edge pixels resolve to a blend", edge >= size / 2),
            ],
            metrics, proof: gpu.proof)
    }
}
