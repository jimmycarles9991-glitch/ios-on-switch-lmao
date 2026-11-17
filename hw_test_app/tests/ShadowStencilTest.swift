import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A fragment function's [[stencil]] output on the raw path: GM20B's fragment program header has no stencil output, so
/// the program tests and writes a stand-in of the stencil inside the interlock (m2v's shader-side stencil, before this
/// m2v's alone). A 32x32 Depth32Float_Stencil8 pass cleared to stencil 16 draws red with each pixel's column x as its
/// reference under LessEqual (pass Replace, stencil failure Zero), then green with x + 256 (low 8 bits x) under Equal
/// (pass IncrementWrap): columns 0-16 end green with stencil x + 1, the rest black with stencil 0. A pass of only a
/// Depth32Float attachment draws the same function blue everywhere: nothing tests its output. PASS: no pixel or stencil
/// texel off and every command buffer on the raw path.
struct ShadowStencilTest: HWTest {
    let id = "gpu.raw.shadow_stencil"
    let name = "A fragment function's [[stencil]] output tested in its program on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let size = 32, cleared: UInt8 = 16

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let n = Self.size
        let colour = try gpu.texture(.rgba8Unorm, n, n)
        let zeta = try gpu.texture(.depth32Float_stencil8, n, n, usage: [.renderTarget])
        let depthOnly = try gpu.texture(.depth32Float, n, n, usage: [.renderTarget])
        let state = try gpu.render(
            fragment: "shadow_stencil_fragment", format: .rgba8Unorm, depth: .depth32Float_stencil8,
            stencil: .depth32Float_stencil8)
        let unattached = try gpu.render(
            fragment: "shadow_stencil_fragment", format: .rgba8Unorm, depth: .depth32Float)
        func depthStencil(
            _ compare: MTLCompareFunction, pass: MTLStencilOperation, fail: MTLStencilOperation
        )
            throws -> MTLDepthStencilState
        {
            let d = MTLDepthStencilDescriptor()
            d.depthCompareFunction = .less
            d.isDepthWriteEnabled = true
            let face = MTLStencilDescriptor()
            face.stencilCompareFunction = compare
            face.depthStencilPassOperation = pass
            face.depthFailureOperation = pass
            face.stencilFailureOperation = fail
            d.frontFaceStencil = face
            d.backFaceStencil = face
            guard let s = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
            return s
        }
        let first = try depthStencil(.lessEqual, pass: .replace, fail: .zero)
        let second = try depthStencil(.equal, pass: .incrementWrap, fail: .keep)
        func quad(_ e: MTLRenderCommandEncoder, z: Float, _ rgba: SIMD4<Float>, offset: UInt32) {
            var corners: [SIMD4<Float>] = [[-1, -1, z, 1], [1, -1, z, 1], [-1, 1, z, 1], [1, 1, z, 1]]
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            var c = rgba
            e.setFragmentBytes(&c, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            var o = offset
            e.setFragmentBytes(&o, length: 4, index: 1)
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
                p.depthAttachment.texture = zeta
                p.depthAttachment.loadAction = .clear
                p.depthAttachment.clearDepth = 1
                p.depthAttachment.storeAction = .store
                p.stencilAttachment.texture = zeta
                p.stencilAttachment.loadAction = .clear
                p.stencilAttachment.clearStencil = UInt32(Self.cleared)
                p.stencilAttachment.storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: p)!
                e.setRenderPipelineState(state)
                e.setDepthStencilState(first)
                quad(e, z: 0.5, [1, 0, 0, 1], offset: 0)
                e.setDepthStencilState(second)
                quad(e, z: 0.25, [0, 1, 0, 1], offset: 256)
                e.endEncoding()
            }.path)
        let plain = try gpu.texture(.rgba8Unorm, n, n)
        paths.append(
            try await gpu.run { commands in
                let p = MTLRenderPassDescriptor()
                p.colorAttachments[0].texture = plain
                p.colorAttachments[0].loadAction = .clear
                p.colorAttachments[0].storeAction = .store
                p.depthAttachment.texture = depthOnly
                p.depthAttachment.loadAction = .clear
                p.depthAttachment.clearDepth = 1
                p.depthAttachment.storeAction = .dontCare
                let e = commands.makeRenderCommandEncoder(descriptor: p)!
                e.setRenderPipelineState(unattached)
                quad(e, z: 0.5, [0, 0, 1, 1], offset: 0)
                e.endEncoding()
            }.path)
        let stencilBytes = try gpu.buffer(n * n)
        try await gpu.run { commands in
            let b = commands.makeBlitCommandEncoder()!
            b.copy(
                from: zeta, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                sourceSize: MTLSize(width: n, height: n, depth: 1), to: stencilBytes, destinationOffset: 0,
                destinationBytesPerRow: n, destinationBytesPerImage: n * n, options: .stencilFromDepthStencil)
            b.endEncoding()
        }
        let pixels = try await gpu.read(colour, bytesPerPixel: 4)
        let blue = try await gpu.read(plain, bytesPerPixel: 4)
        let s = stencilBytes.contents().bindMemory(to: UInt8.self, capacity: n * n)
        var colourWrong = 0, stencilWrong = 0, unattachedWrong = 0
        for y in 0..<n {
            for x in 0..<n {
                let kept = x <= Int(Self.cleared)
                let at = 4 * (x + n * y)
                colourWrong += Array(pixels[at..<at + 4]) == (kept ? [0, 255, 0, 255] : [0, 0, 0, 255]) ? 0 : 1
                stencilWrong += s[x + n * y] == (kept ? UInt8(x + 1) : 0) ? 0 : 1
                unattachedWrong += Array(blue[at..<at + 4]) == [0, 0, 255, 255] ? 0 : 1
            }
        }
        var metrics = Metrics()
        metrics.set("paths", paths.joined(separator: ","))
        metrics.set("colour_wrong", colourWrong)
        metrics.set("stencil_wrong", stencilWrong)
        metrics.set("unattached_wrong", unattachedWrong)
        metrics.set("stencil_x0_x16_x17", "\(s[0])/\(s[16])/\(s[17])")
        metrics.set("colour_x16_x17", "\(pixels[4 * 16 + 1])/\(pixels[4 * 17])")
        var failures: [String] = []
        if paths.contains(where: { $0 != "raw" }) { failures.append("a pass went off the raw path") }
        if colourWrong != 0 { failures.append("\(colourWrong) pixels wrong") }
        if stencilWrong != 0 { failures.append("\(stencilWrong) stencil texels wrong") }
        if unattachedWrong != 0 { failures.append("\(unattachedWrong) pixels wrong without a stencil") }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}
