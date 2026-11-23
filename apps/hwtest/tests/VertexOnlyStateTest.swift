import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A render state of no fragment function (RenderBox's path_edges states are of this kind): drawn by the driver with
/// its pixel stage off (drivers/TegraGM20BMetal/stage_switches.h), it writes depth and stencil and no
/// colour. Left half: quad_vertex alone at z 0.5, stencil replaced with 1. Then a state Metal does not rasterize draws
/// the whole target at z 0.1, which changes nothing. No command buffer may fail (failed_cbs=).
struct VertexOnlyStateTest: HWTest {
    let id = "gpu.gm20b.vertex_only_state"
    let name = "A state of no fragment function: depth and stencil written, colour untouched"
    let subsystem = Subsystem.metal

    func vertexOnly(_ gpu: GPU, rasterize: Bool) throws -> MTLRenderPipelineState {
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = gpu.library.makeFunction(name: "quad_vertex")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.colorAttachments[0].writeMask = []
        descriptor.depthAttachmentPixelFormat = .depth32Float_stencil8
        descriptor.stencilAttachmentPixelFormat = .depth32Float_stencil8
        descriptor.isRasterizationEnabled = rasterize
        return try gpu.device.makeRenderPipelineState(descriptor: descriptor)
    }

    func depthState(_ device: MTLDevice) -> MTLDepthStencilState {
        let descriptor = MTLDepthStencilDescriptor()
        descriptor.depthCompareFunction = .less
        descriptor.isDepthWriteEnabled = true
        let face = MTLStencilDescriptor()
        face.stencilCompareFunction = .always
        face.depthStencilPassOperation = .replace
        descriptor.frontFaceStencil = face
        descriptor.backFaceStencil = face
        return device.makeDepthStencilState(descriptor: descriptor)!
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        let size = 64
        let color = try gpu.texture(.rgba8Unorm, size, size)
        let depth = try gpu.texture(.depth32Float_stencil8, size, size, usage: [.renderTarget])
        let drawn = try vertexOnly(gpu, rasterize: true)
        let unrasterized = try vertexOnly(gpu, rasterize: false)
        let depthStencil = depthState(gpu.device)
        let path = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = color
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 1, blue: 0, alpha: 1)
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = .store
            pass.stencilAttachment.texture = depth
            pass.stencilAttachment.loadAction = .clear
            pass.stencilAttachment.clearStencil = 0
            pass.stencilAttachment.storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setDepthStencilState(depthStencil)
            encoder.setStencilReferenceValue(1)
            encoder.setRenderPipelineState(drawn)
            GPU.draw(encoder, corners: GPU.quad(x0: -1, y0: -1, x1: 0, y1: 1, z: 0.5), color: SIMD4(1, 0, 0, 1))
            encoder.setRenderPipelineState(unrasterized)
            encoder.setStencilReferenceValue(2)
            GPU.draw(encoder, corners: GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.1), color: SIMD4(0, 1, 0, 1))
            encoder.endEncoding()
        }.path
        let pixels = try await gpu.read(color, bytesPerPixel: 4)
        let depths = try await gpu.read(depth, bytesPerPixel: 4, options: .depthFromDepthStencil)
        let stencils = try await gpu.read(depth, bytesPerPixel: 1, options: .stencilFromDepthStencil)
        var (wrongColor, wrongDepth, wrongStencil) = (0, 0, 0)
        for y in 0 ..< size {
            for x in 0 ..< size {
                let left = x < size / 2
                if Array(pixels.pixel(x, y, width: size)) != [0, 255, 0, 255] { wrongColor += 1 }
                if depths.loaded(as: Float.self, at: y * size + x) != (left ? 0.5 : 1) { wrongDepth += 1 }
                if stencils[y * size + x] != (left ? 1 : 0) { wrongStencil += 1 }
            }
        }
        var metrics = Metrics()
        metrics.set("wrong_color", wrongColor)
        metrics.set("wrong_depth", wrongDepth)
        metrics.set("wrong_stencil", wrongStencil)
        metrics.set("corner_color", Array(pixels.pixel(0, 0, width: size)).map(String.init).joined(separator: ","))
        metrics.set("path", path)
        let failed = FailedCounts.judged(
            before: before, after: GM20BFacade.counters(of: gpu.device), own: gpu.paths, &metrics
        )
        return .judged(
            [
                ("colour untouched (0,255,0,255)", wrongColor == 0), ("depth 0.5 | 1", wrongDepth == 0),
                ("stencil 1 | 0", wrongStencil == 0), ("drawn by raw", path == "gm20b"), failed,
            ], metrics, proof: gpu.proof
        )
    }
}
