import Foundation
import HWTestCore
import Metal

// A heap texture drawn after its backing was purged keeps its contents. A white border sampler is one raw refuses
// (call.sampler) while the facade wraps an m2v device, which then makes the texture's twin; with no m2v device the same
// draw has to be raw's. Sampling stays inside the texture.
struct HeapTwinScene: GPUScene {
    let id = "gpu.heap.m2v_target"
    let name = "A heap texture drawn by an m2v command buffer, before and after the heap is purged"

    static let (width, height) = (140, 143)
    static let color: [UInt8] = [51, 153, 204, 255]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let heapDescriptor = MTLHeapDescriptor()
        heapDescriptor.size = 2 << 20
        heapDescriptor.storageMode = .private
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: Self.width, height: Self.height, mipmapped: false)
        descriptor.storageMode = .private
        descriptor.usage = [.renderTarget, .shaderRead]
        let samplerDescriptor = MTLSamplerDescriptor()
        samplerDescriptor.sAddressMode = .clampToBorderColor
        samplerDescriptor.tAddressMode = .clampToBorderColor
        samplerDescriptor.borderColor = .opaqueWhite
        guard let sampler = gpu.device.makeSamplerState(descriptor: samplerDescriptor)
        else {
            result.expect(false, "the white border sampler returned nil")
            return result
        }
        let source = try gpu.texture(.rgba8Unorm, 8, 8, usage: [.shaderRead])
        try await gpu.upload(Array([[UInt8]](repeating: Self.color, count: 64).joined()), to: source, bytesPerRow: 32)
        let fill = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let sampled = try gpu.render(vertex: "textured_vertex", fragment: "sampled_fragment", format: .bgra8Unorm)
        let want = Raster(width: Self.width, height: Self.height, channels: 4) { _, _, c in
            Float(Self.color[c]) / 255
        }
        var paths: [String] = []
        for round in ["fresh", "purged, non-volatile again", "purged, still volatile"] {
            guard let heap = gpu.device.makeHeap(descriptor: heapDescriptor),
                let target = heap.makeTexture(descriptor: descriptor)
            else {
                result.expect(false, "makeHeap or its texture returned nil")
                return result
            }
            try await draw(gpu, fill, into: target) { encoder in
                GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(1, 0, 1, 1))
            }
            if round != "fresh" {
                heap.setPurgeableState(.empty)
                if round == "purged, non-volatile again" { heap.setPurgeableState(.nonVolatile) }
            }
            let timing = try await draw(gpu, sampled, into: target) { encoder in
                var corners = GPU.fullScreen
                var uvs: [SIMD2<Float>] = [SIMD2(0, 1), SIMD2(1, 1), SIMD2(0, 0), SIMD2(1, 0)]
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setVertexBytes(&uvs, length: MemoryLayout<SIMD2<Float>>.stride * 4, index: 1)
                encoder.setFragmentTexture(source, index: 0)
                encoder.setFragmentSamplerState(sampler, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            paths.append(timing.path)
            if RawFacade.kind(of: gpu.device) == "raw" {
                let route = RawFacade.wrapsM2V(gpu.device) ? "m2v:" : "raw"
                result.expect(timing.path.hasPrefix(route), "the sampled draw took \(timing.path), not \(route)")
            }
            result.compare(round, try await gpu.raster(target, .bgra8Unorm), want, tolerance: 1.51 / 255)
            heap.setPurgeableState(.nonVolatile)
        }
        result.metrics.set("paths", paths.joined(separator: ","))
        return result
    }

    @discardableResult
    private func draw(
        _ gpu: GPU, _ pipeline: MTLRenderPipelineState, into target: MTLTexture,
        _ body: @escaping (MTLRenderCommandEncoder) -> Void
    ) async throws -> GPU.Timing {
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            body(encoder)
            encoder.endEncoding()
        }
    }
}
