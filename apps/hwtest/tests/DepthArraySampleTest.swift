import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A depth 2D array sampled whole by a draw (enc.notic before: a stored depth array had no texture header, its samples
/// m2v's): drivers/TegraGM20BMetal/multisample.cpp createOwnedZeta gives a ShaderRead one NIL's array
/// header. Each layer is cleared to its own depth, then pixel x reads layer x. The draw's command buffer must stay raw;
/// the clears' path is reported.
struct DepthArraySampleTest: HWTest {
    let id = "gpu.gm20b.depth_array_sample"
    let name = "A depth 2D array sampled by a draw reads each layer's depth, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let d = MTLTextureDescriptor()
        d.pixelFormat = .depth32Float
        d.textureType = .type2DArray
        d.width = 8
        d.height = 8
        d.arrayLength = 3
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .private
        guard let depths = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no depth array") }
        let scratch = try gpu.texture(.rgba8Unorm, 8, 8)
        let fill = try await gpu.run { commands in
            for slice in 0 ..< 3 {
                let p = MTLRenderPassDescriptor()
                p.colorAttachments[0].texture = scratch
                p.colorAttachments[0].loadAction = .clear
                p.colorAttachments[0].storeAction = .dontCare
                p.depthAttachment.texture = depths
                p.depthAttachment.slice = slice
                p.depthAttachment.loadAction = .clear
                p.depthAttachment.clearDepth = 0.25 * Double(slice + 1)
                p.depthAttachment.storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: p)!.endEncoding()
            }
        }.path
        let target = try gpu.texture(.r32Float, 3, 1)
        let state = try gpu.render(fragment: "view_read_depth_layers", format: .r32Float)
        let path = try await gpu.run { commands in
            let p = MTLRenderPassDescriptor()
            p.colorAttachments[0].texture = target
            p.colorAttachments[0].loadAction = .clear
            p.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: p)!
            e.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setFragmentTexture(depths, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
        }.path
        let bytes = try await gpu.read(target, bytesPerPixel: 4)
        let values = (0 ..< 3).map { bytes.loaded(as: Float.self, at: $0) }
        var metrics = Metrics()
        metrics.set("fill_path", fill)
        metrics.set("path", path)
        metrics.set("depths", values.map { String($0) }.joined(separator: ","))
        var wrong: [String] = []
        if values != [0.25, 0.5, 0.75] { wrong.append("depths") }
        if path != "gm20b" { wrong.append("path") }
        return wrong.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(wrong.joined(separator: ","), metrics)
    }
}
