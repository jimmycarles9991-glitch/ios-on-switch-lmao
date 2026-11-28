import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.gm20b.zcull's side_effects arm: a near full-screen quad (z 0.25, LESS, from a 1.0 clear), then far quads (z 0.75)
/// whose fragment function adds to a device counter and asks for no early fragment tests: Metal runs it for every far
/// fragment, hidden or not
public enum ZcullSideEffects {
    public static let farQuads = 64

    /// The pass's encode; `count` the counter the far quads add to
    public static func pass(
        _ gpu: GPU, target: MTLTexture, depth: MTLTexture, count: MTLBuffer
    ) throws -> (MTLCommandBuffer) -> Void {
        let near = try gpu.render(fragment: "exact_fragment", format: target.pixelFormat, depth: .depth32Float)
        let far = try gpu.render(
            fragment: "zcull_side_effect_fragment",
            format: target.pixelFormat,
            depth: .depth32Float
        )
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .less
        d.isDepthWriteEnabled = true
        let less = gpu.device.makeDepthStencilState(descriptor: d)!
        let (w, h) = (target.width, target.height)
        return { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = .dontCare
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setDepthStencilState(less)
            encoder.setRenderPipelineState(near)
            GPU.draw(
                encoder,
                corners: GPU.pixelQuad(0, 0, w, h, width: w, height: h, z: 0.25),
                color: SIMD4(1, 0, 0, 1)
            )
            encoder.setRenderPipelineState(far)
            encoder.setFragmentBuffer(count, offset: 0, index: 1)
            for _ in 0 ..< farQuads {
                GPU.draw(
                    encoder,
                    corners: GPU.pixelQuad(0, 0, w, h, width: w, height: h, z: 0.75),
                    color: SIMD4(0, 1, 0, 1)
                )
            }
            encoder.endEncoding()
        }
    }
}
