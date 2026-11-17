import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A depth attachment whose store action the pass leaves Unknown and the encoder decides (setDepthStoreAction:): such a
/// pass was pass.ds, m2v's. The first pass writes depth 0.5 over the left half and decides Store; the second loads it and
/// draws red at 0.75 under a Less test, which only the right half passes. Both passes run on the raw path.
struct DeferredZetaStoreTest: HWTest {
    let id = "gpu.raw.deferred_zeta_store"
    let name = "A depth store action the encoder decides keeps the depth, on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let n = 32
        let colour = try gpu.texture(.rgba8Unorm, n, n)
        let depth = try gpu.texture(.depth32Float, n, n, usage: [.renderTarget])
        let state = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm, depth: .depth32Float)
        func depthState(_ compare: MTLCompareFunction, write: Bool) throws -> MTLDepthStencilState {
            let d = MTLDepthStencilDescriptor()
            d.depthCompareFunction = compare
            d.isDepthWriteEnabled = write
            guard let s = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
            return s
        }
        let write = try depthState(.always, write: true), less = try depthState(.less, write: false)
        func quad(_ e: MTLRenderCommandEncoder, x0: Float, x1: Float, z: Float, _ rgba: SIMD4<Float>) {
            var corners: [SIMD4<Float>] = [[x0, -1, z, 1], [x1, -1, z, 1], [x0, 1, z, 1], [x1, 1, z, 1]]
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            var c = rgba
            e.setFragmentBytes(&c, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        }
        var paths: [String] = []
        paths.append(
            try await gpu.run { commands in
                let p = MTLRenderPassDescriptor()
                p.colorAttachments[0].texture = colour
                p.colorAttachments[0].loadAction = .clear
                p.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                p.colorAttachments[0].storeAction = .store
                p.depthAttachment.texture = depth
                p.depthAttachment.loadAction = .clear
                p.depthAttachment.clearDepth = 1
                p.depthAttachment.storeAction = .unknown
                let e = commands.makeRenderCommandEncoder(descriptor: p)!
                e.setRenderPipelineState(state)
                e.setDepthStencilState(write)
                quad(e, x0: -1, x1: 0, z: 0.5, [0, 0, 0, 1])
                e.setDepthStoreAction(.store)
                e.endEncoding()
            }.path)
        paths.append(
            try await gpu.run { commands in
                let p = MTLRenderPassDescriptor()
                p.colorAttachments[0].texture = colour
                p.colorAttachments[0].loadAction = .load
                p.colorAttachments[0].storeAction = .store
                p.depthAttachment.texture = depth
                p.depthAttachment.loadAction = .load
                p.depthAttachment.storeAction = .dontCare
                let e = commands.makeRenderCommandEncoder(descriptor: p)!
                e.setRenderPipelineState(state)
                e.setDepthStencilState(less)
                quad(e, x0: -1, x1: 1, z: 0.75, [1, 0, 0, 1])
                e.endEncoding()
            }.path)
        let pixels = try await gpu.read(colour, bytesPerPixel: 4)
        var wrong = 0
        for y in 0..<n {
            for x in 0..<n {
                let at = 4 * (x + n * y)
                let want: [UInt8] = x < n / 2 ? [0, 0, 0, 255] : [255, 0, 0, 255]
                wrong += Array(pixels[at..<at + 4]) == want ? 0 : 1
            }
        }
        var metrics = Metrics()
        metrics.set("paths", paths.joined(separator: ","))
        metrics.set("colour_wrong", wrong)
        var failures: [String] = []
        if paths.contains(where: { $0 != "raw" }) { failures.append("a pass went off the raw path") }
        if wrong != 0 { failures.append("\(wrong) pixels wrong: the decided store lost the depth") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
