import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Tile shading on a device that does not claim Apple4 (tile shaders and imageblocks are Apple4 in Metal's feature set
/// tables): a tile render state is refused at creation with an error, and a tile dispatch inside a raw render pass is
/// answered zero (the error line raw:call.tile) while the pass and its command buffer stay on the raw path and complete.
/// Before, both went to m2v.
struct TileRefusedTest: NativeObjectTest {
    let id = "gpu.raw.tile_refused"
    let name = "Tile shading is refused as a device without Apple4 refuses it, on the raw path"

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct Tile { half4 colour [[color(0)]]; };
        kernel void tile_clear(imageblock<Tile, imageblock_layout_implicit> block, ushort2 at [[thread_position_in_threadgroup]]) {
            block.write(Tile{half4(1)}, at);
        }
        """

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let apple4 = device.supportsFamily(.apple4)
        metrics.set("apple4", apple4 ? "yes" : "no")
        if apple4 { work.failures.append("the device claims Apple4, whose tile shaders it has no form of") }
        if let library = try? await device.makeLibrary(source: Self.source, options: nil),
            let function = library.makeFunction(name: "tile_clear")
        {
            let d = MTLTileRenderPipelineDescriptor()
            d.tileFunction = function
            d.colorAttachments[0].pixelFormat = .bgra8Unorm
            do {
                _ = try device.makeRenderPipelineState(tileDescriptor: d, options: [], reflection: nil)
                metrics.set("tile_state", "made")
                work.failures.append("a tile render state was made")
            } catch {
                metrics.set("tile_state", "refused:\((error as NSError).code)")
            }
        } else {
            metrics.set("tile_state", "no_library")
        }
        let target = try gpu.texture(.bgra8Unorm, 16, 16)
        let state = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 1, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            var colour = SIMD4<Float>(1, 0, 0, 1)
            encoder.setFragmentBytes(&colour, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.dispatchThreadsPerTile(MTLSize(width: 16, height: 16, depth: 1))
            encoder.endEncoding()
        }
        work.paths.append(timing.path)
        metrics.set("dispatch_path", timing.path)
        return work
    }
}
