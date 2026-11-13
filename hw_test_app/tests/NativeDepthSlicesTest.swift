import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The slices of a stored depth 2D array, a pass each: slice k cleared to depth 1 where k is even and 0 where odd and
/// stored, then loaded by a pass over the same slice of a colour array that draws a quad at depth 0.5 under a less
/// compare. Only the even slices' quads pass, so each slice's pass read the depth its own clear stored. The depth array
/// is m2v's allocation the raw path renders in place (its twin from the start), so it is no object of the verdict.
struct NativeDepthSlicesTest: NativeObjectTest {
    let id = "metal.native.depth_slices"
    let name = "Each slice of a stored depth array keeps the depth its own pass stored"

    static let layers = 4, size = 32

    static func depthArray(_ gpu: GPU) throws -> MTLTexture {
        let d = MTLTextureDescriptor()
        d.textureType = .type2DArray
        d.pixelFormat = .depth32Float
        d.width = size
        d.height = size
        d.arrayLength = layers
        d.usage = .renderTarget
        d.storageMode = .private
        guard let t = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no depth 2D array") }
        return t
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let depth = try Self.depthArray(gpu), color = try Layering.array(gpu, Self.size, layers: Self.layers)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm, depth: .depth32Float)
        let descriptor = MTLDepthStencilDescriptor()
        descriptor.depthCompareFunction = .less
        descriptor.isDepthWriteEnabled = false
        guard let less = gpu.device.makeDepthStencilState(descriptor: descriptor) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        var work = NativeWork(objects: [color, less])
        let black: [UInt8] = [0, 0, 0, 255], red: [UInt8] = [255, 0, 0, 255]
        let timing = try await gpu.run { commands in
            for slice in 0..<Self.layers {
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = color
                pass.colorAttachments[0].slice = slice
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = depth
                pass.depthAttachment.slice = slice
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = slice % 2 == 0 ? 1 : 0
                pass.depthAttachment.storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
            for slice in 0..<Self.layers {
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = color
                pass.colorAttachments[0].slice = slice
                pass.colorAttachments[0].loadAction = .load
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = depth
                pass.depthAttachment.slice = slice
                pass.depthAttachment.loadAction = .load
                pass.depthAttachment.storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(pipeline)
                e.setDepthStencilState(less)
                GPU.draw(e, corners: GPU.fullScreen, color: SIMD4(1, 0, 0, 1))
                e.endEncoding()
            }
        }
        work.paths.append(timing.path)
        let wrong = try await Layering.wrong(gpu, color, paths: &work.paths) { layer, _, _ in
            layer % 2 == 0 ? red : black
        }
        metrics.set("wrong_texels", wrong)
        if wrong != 0 { work.failures.append("\(wrong) texels drawn against another slice's depth") }
        return work
    }
}
