import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.gm20b.zcull's fragment_stores arm (drivers/TegraGM20BMetal/zcull.h zcullInvalidates: a program
/// that can write memory drops its pass's region): a 1.0-cleared memoryless Depth32Float, a near full-screen quad at
/// 0.25 (LESS, writes), then one far full-screen quad at 0.75 whose fragment function adds to a device counter with no
/// early fragment tests (ZcullSideEffect.metal). Metal runs it for every far fragment: 1280 x 720 = 921,600 on an M4
/// Max, memoryless or private depth alike (hwtests12 mac/gpu.gm20b.zcull). The pass gets its region
/// (`stores_zcull_passes` 1) and drops it at the storing program (`stores_zcull_invalidations` 1). Red: a count short
/// of the fragments (zcull culled fragments whose stores Metal promises: writesMemory missed the store, or the
/// invalidation came after the draw).
extension ZcullTest {
    func fragmentStores(
        _ gpu: GPU, _ target: MTLTexture, _ metrics: inout Metrics, _ wrong: inout [String], _ paths: inout Set<String>
    ) async throws {
        let (w, h) = (target.width, target.height)
        let depth = try gpu.texture(.depth32Float, w, h, usage: [.renderTarget], storage: .memoryless)
        let near = try gpu.render(fragment: "exact_fragment", format: target.pixelFormat, depth: .depth32Float)
        let far = try gpu.render(
            fragment: "zcull_side_effect_fragment", format: target.pixelFormat, depth: .depth32Float
        )
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .less
        d.isDepthWriteEnabled = true
        guard let less = gpu.device.makeDepthStencilState(descriptor: d) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        let count = try gpu.buffer([UInt32(0)])
        let before = GM20BFacade.counters(of: gpu.device)
        paths.insert(
            try await gpu.run { commands in
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
                GPU.draw(encoder, corners: GPU.pixelQuad(0, 0, w, h, width: w, height: h, z: 0.25), color: Self.red)
                encoder.setRenderPipelineState(far)
                encoder.setFragmentBuffer(count, offset: 0, index: 1)
                GPU.draw(encoder, corners: GPU.pixelQuad(0, 0, w, h, width: w, height: h, z: 0.75), color: Self.green)
                encoder.endEncoding()
            }.path
        )
        let after = GM20BFacade.counters(of: gpu.device)
        let delta = { (key: String) in (after[key] ?? 0) - (before[key] ?? 0) }
        let counted = Int(count.contents().load(as: UInt32.self))
        metrics.set("stores_counter", counted)
        metrics.set("stores_expected", w * h)
        metrics.set("stores_zcull_passes", delta("zcull_passes"))
        metrics.set("stores_zcull_invalidations", delta("zcull_invalidations"))
        if counted != w * h { wrong.append("fragment_stores_count") }
        if delta("zcull_passes") != 1 { wrong.append("fragment_stores_zcull_passes") }
        if delta("zcull_invalidations") != 1 { wrong.append("fragment_stores_zcull_invalidations") }
    }
}
