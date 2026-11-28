import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// A 4x multisampled Depth16Unorm texture and Stencil8 texture as one pass's attachments (pass.ds took any multisampled
/// Depth16Unorm pair to m2v; the driver renders a multisampled Depth32Float_Stencil8 of the pass's). Cleared and not kept: a
/// red quad at depth 0.5 over the left half writing stencil 1, a green one at 0.7 over the whole target under Less, a blue one
/// over the whole target where the stencil is 1; resolved colour blue on the left, green on the right. Kept: a pass stores a
/// depth of 0.5 over the left half, the next loads it and draws green at 0.7 under Less (each sample's depth converted
/// out and back in by the pack's multisampled conversion states); resolved colour black on the left, green on the right.
/// Every pass on the driver.
struct SeparateDepth16StencilMultisampleTest: HWTest {
    let id = "gpu.gm20b.separate_depth16_stencil_ms"
    let name = "A multisampled Depth16Unorm and Stencil8 pair, transient and kept across passes, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let n = 16, samples = 4
        let colour = try gpu.texture(.rgba8Unorm, n, n, samples: samples)
        let resolved = try gpu.texture(.rgba8Unorm, n, n)
        let depth = try gpu.texture(.depth16Unorm, n, n, usage: [.renderTarget], samples: samples)
        let stencil = try gpu.texture(.stencil8, n, n, usage: [.renderTarget], samples: samples)
        let state = try gpu.render(
            fragment: "solid_fragment", format: .rgba8Unorm, samples: samples, depth: .depth16Unorm, stencil: .stencil8
        )
        func ds(
            _ compare: MTLCompareFunction, stencil test: MTLCompareFunction, replace: Bool
        ) throws -> MTLDepthStencilState {
            let d = MTLDepthStencilDescriptor()
            d.depthCompareFunction = compare
            d.isDepthWriteEnabled = true
            let face = MTLStencilDescriptor()
            face.stencilCompareFunction = test
            face.depthStencilPassOperation = replace ? .replace : .keep
            d.frontFaceStencil = face
            d.backFaceStencil = face
            guard let s = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
            return s
        }
        let mark = try ds(.always, stencil: .always, replace: true)
        let behind = try ds(.less, stencil: .always, replace: false)
        let marked = try ds(.always, stencil: .equal, replace: false)
        let path = try await gpu.run { commands in
            let p = MTLRenderPassDescriptor()
            p.colorAttachments[0].texture = colour
            p.colorAttachments[0].resolveTexture = resolved
            p.colorAttachments[0].loadAction = .clear
            p.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            p.colorAttachments[0].storeAction = .multisampleResolve
            p.depthAttachment.texture = depth
            p.depthAttachment.loadAction = .clear
            p.depthAttachment.clearDepth = 1
            p.depthAttachment.storeAction = .dontCare
            p.stencilAttachment.texture = stencil
            p.stencilAttachment.loadAction = .clear
            p.stencilAttachment.storeAction = .dontCare
            let e = commands.makeRenderCommandEncoder(descriptor: p)!
            e.setRenderPipelineState(state)
            e.setStencilReferenceValue(1)
            func quad(_ x1: Float, _ z: Float, _ rgba: SIMD4<Float>, _ s: MTLDepthStencilState) {
                e.setDepthStencilState(s)
                var corners: [SIMD4<Float>] = [[-1, -1, z, 1], [x1, -1, z, 1], [-1, 1, z, 1], [x1, 1, z, 1]]
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                var c = rgba
                e.setFragmentBytes(&c, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            quad(0, 0.5, [1, 0, 0, 1], mark)
            quad(1, 0.7, [0, 1, 0, 1], behind)
            quad(1, 0.9, [0, 0, 1, 1], marked)
            e.endEncoding()
        }.path
        func wrong(_ left: [UInt8], _ right: [UInt8]) async throws -> Int {
            let pixels = try await gpu.read(resolved, bytesPerPixel: 4)
            var count = 0
            for y in 0 ..< n {
                for x in 0 ..< n {
                    count += Array(pixels[4 * (y * n + x) ..< 4 * (y * n + x) + 4]) == (x < n / 2 ? left : right) ? 0 :
                        1
                }
            }
            return count
        }
        let transientWrong = try await wrong([0, 0, 255, 255], [0, 255, 0, 255])
        func kept(_ load: Bool, _ commands: MTLCommandBuffer) {
            let p = MTLRenderPassDescriptor()
            p.colorAttachments[0].texture = colour
            p.colorAttachments[0].resolveTexture = resolved
            p.colorAttachments[0].loadAction = .clear
            p.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            p.colorAttachments[0].storeAction = .multisampleResolve
            p.depthAttachment.texture = depth
            p.depthAttachment.loadAction = load ? .load : .clear
            p.depthAttachment.clearDepth = 1
            p.depthAttachment.storeAction = .store
            p.stencilAttachment.texture = stencil
            p.stencilAttachment.loadAction = .clear
            p.stencilAttachment.storeAction = .dontCare
            let e = commands.makeRenderCommandEncoder(descriptor: p)!
            e.setRenderPipelineState(state)
            e.setDepthStencilState(load ? behind : mark)
            let (x1, z, rgba): (Float, Float, SIMD4<Float>) = load ? (1, 0.7, [0, 1, 0, 1]) : (0, 0.5, [0, 0, 0, 1])
            var corners: [SIMD4<Float>] = [[-1, -1, z, 1], [x1, -1, z, 1], [-1, 1, z, 1], [x1, 1, z, 1]]
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            var c = rgba
            e.setFragmentBytes(&c, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
        }
        let keptPaths = [
            try await gpu.run { kept(false, $0) }.path,
            try await gpu.run { kept(true, $0) }.path,
        ]
        let keptWrong = try await wrong([0, 0, 0, 255], [0, 255, 0, 255])
        var metrics = Metrics()
        metrics.set("path", path)
        metrics.set("colour_wrong", transientWrong)
        metrics.set("kept_paths", keptPaths.joined(separator: ","))
        metrics.set("kept_wrong", keptWrong)
        var failures: [String] = []
        if path != "gm20b" || keptPaths.contains(where: { $0 != "gm20b" }) {
            failures.append("a pass went off the driver")
        }
        if transientWrong != 0 { failures.append("\(transientWrong) resolved pixels wrong") }
        if keptWrong != 0 { failures.append("\(keptWrong) pixels wrong after the kept depth") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
