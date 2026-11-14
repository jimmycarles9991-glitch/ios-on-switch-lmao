import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A pass whose depth and stencil attachments are two textures, a Depth32Float and a Stencil8 (boot 21 pass.ds a=252 b=253:
/// such a pass went to m2v). The first pass clears both, writes depth 0.5 and stencil 0x5A over the left half and stores
/// them; the second loads both and draws red at depth 0.25 where the stencil is 0x5A, then green at depth 0.75 where it is,
/// which the depth test keeps out. The colour is red on the left and black on the right, and each texture read back holds
/// what the first pass stored. The two passes' command buffers run on the raw path.
struct SeparateDepthStencilTest: HWTest {
    let id = "gpu.raw.separate_depth_stencil"
    let name = "Depth32Float and Stencil8 textures as one pass's attachments, on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let size = 32

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let n = Self.size
        let colour = try gpu.texture(.rgba8Unorm, n, n)
        let depth = try gpu.texture(.depth32Float, n, n, usage: [.renderTarget])
        let stencil = try gpu.texture(.stencil8, n, n, usage: [.renderTarget])
        let state = try gpu.render(
            fragment: "solid_fragment", format: .rgba8Unorm, depth: .depth32Float, stencil: .stencil8)
        func depthStencil(
            _ compare: MTLCompareFunction, write: Bool, stencil: MTLCompareFunction, replace: Bool
        )
            throws -> MTLDepthStencilState
        {
            let d = MTLDepthStencilDescriptor()
            d.depthCompareFunction = compare
            d.isDepthWriteEnabled = write
            let face = MTLStencilDescriptor()
            face.stencilCompareFunction = stencil
            face.depthStencilPassOperation = replace ? .replace : .keep
            d.frontFaceStencil = face
            d.backFaceStencil = face
            guard let s = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
            return s
        }
        let writeBoth = try depthStencil(.always, write: true, stencil: .always, replace: true)
        let tested = try depthStencil(.less, write: false, stencil: .equal, replace: false)
        func quad(_ encoder: MTLRenderCommandEncoder, x0: Float, x1: Float, z: Float, _ rgba: SIMD4<Float>) {
            var corners: [SIMD4<Float>] = [[x0, -1, z, 1], [x1, -1, z, 1], [x0, 1, z, 1], [x1, 1, z, 1]]
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            var c = rgba
            encoder.setFragmentBytes(&c, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        }
        func pass(_ load: Bool) -> MTLRenderPassDescriptor {
            let p = MTLRenderPassDescriptor()
            p.colorAttachments[0].texture = colour
            p.colorAttachments[0].loadAction = load ? .load : .clear
            p.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            p.colorAttachments[0].storeAction = .store
            p.depthAttachment.texture = depth
            p.depthAttachment.loadAction = load ? .load : .clear
            p.depthAttachment.clearDepth = 1
            p.depthAttachment.storeAction = .store
            p.stencilAttachment.texture = stencil
            p.stencilAttachment.loadAction = load ? .load : .clear
            p.stencilAttachment.clearStencil = 0
            p.stencilAttachment.storeAction = .store
            return p
        }
        var metrics = Metrics()
        var failures: [String] = []
        var paths: [String] = []
        paths.append(
            try await gpu.run { commands in
                let e = commands.makeRenderCommandEncoder(descriptor: pass(false))!
                e.setRenderPipelineState(state)
                e.setDepthStencilState(writeBoth)
                e.setStencilReferenceValue(0x5A)
                quad(e, x0: -1, x1: 0, z: 0.5, [0, 0, 1, 1])
                e.endEncoding()
            }.path)
        paths.append(
            try await gpu.run { commands in
                let e = commands.makeRenderCommandEncoder(descriptor: pass(true))!
                e.setRenderPipelineState(state)
                e.setDepthStencilState(tested)
                e.setStencilReferenceValue(0x5A)
                quad(e, x0: -1, x1: 1, z: 0.25, [1, 0, 0, 1])
                quad(e, x0: -1, x1: 1, z: 0.75, [0, 1, 0, 1])
                e.endEncoding()
            }.path)
        metrics.set("paths", paths.joined(separator: ","))
        if paths.contains(where: { $0 != "raw" }) { failures.append("a pass went off the raw path") }
        let pixels = try await gpu.read(colour, bytesPerPixel: 4)
        var colourWrong = 0
        for y in 0..<n {
            for x in 0..<n {
                let at = 4 * (x + n * y)
                let want: [UInt8] = x < n / 2 ? [255, 0, 0, 255] : [0, 0, 0, 255]
                colourWrong += Array(pixels[at..<at + 4]) == want ? 0 : 1
            }
        }
        metrics.set("colour_wrong", colourWrong)
        metrics.set(
            "colour_left_right",
            "\(pixels[4 * 2]).\(pixels[4 * 2 + 1])/\(pixels[4 * (n - 2)]).\(pixels[4 * (n - 2) + 1])")
        if colourWrong != 0 { failures.append("\(colourWrong) colour pixels wrong") }
        let depthBytes = try gpu.buffer(n * n * 4)
        let stencilBytes = try gpu.buffer(n * n)
        let readback = try await gpu.run { commands in
            let b = commands.makeBlitCommandEncoder()!
            b.copy(
                from: depth, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                sourceSize: MTLSize(width: n, height: n, depth: 1), to: depthBytes, destinationOffset: 0,
                destinationBytesPerRow: n * 4, destinationBytesPerImage: n * n * 4)
            b.copy(
                from: stencil, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                sourceSize: MTLSize(width: n, height: n, depth: 1), to: stencilBytes, destinationOffset: 0,
                destinationBytesPerRow: n, destinationBytesPerImage: n * n)
            b.endEncoding()
        }
        metrics.set("readback_path", readback.path)
        let d = depthBytes.contents().bindMemory(to: Float.self, capacity: n * n)
        let s = stencilBytes.contents().bindMemory(to: UInt8.self, capacity: n * n)
        var depthWrong = 0
        var stencilWrong = 0
        for y in 0..<n {
            for x in 0..<n {
                let left = x < n / 2
                depthWrong += d[x + n * y] == (left ? 0.5 : 1.0) ? 0 : 1
                stencilWrong += s[x + n * y] == (left ? 0x5A : 0) ? 0 : 1
            }
        }
        metrics.set("depth_wrong", depthWrong)
        metrics.set("stencil_wrong", stencilWrong)
        metrics.set("depth_left_right", "\(d[2])/\(d[n - 2])")
        metrics.set("stencil_left_right", "\(s[2])/\(s[n - 2])")
        if depthWrong != 0 { failures.append("\(depthWrong) depth texels wrong") }
        if stencilWrong != 0 { failures.append("\(stencilWrong) stencil texels wrong") }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}
