import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A fragment function's [[stencil]] output whose stencil write follows the depth test (a depth compare other than Always,
/// a depth-failure operation other than the pass one): the hardware tests depth after the program, so the raw path draws
/// the state's variant whose program runs both tests on stand-ins (m2v refused these draws). A 32x32
/// Depth32Float_Stencil8 pass cleared to depth 1 and stencil 16 writes depth 0.5 over the left half, then draws green at
/// depth 0.75 under Less with each column x as reference, stencil Always, IncrementClamp on depth failure and Replace on
/// pass: the left half keeps its colour with stencil 17, the right is green with stencil x. PASS: no pixel or texel off
/// and every command buffer on the raw path.
struct ShadowStencilDepthTest: HWTest {
    let id = "gpu.raw.shadow_stencil_depth"
    let name =
        "A [[stencil]] output whose stencil write follows the depth test, both tests in its program on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let size = 32

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let n = Self.size
        let colour = try gpu.texture(.rgba8Unorm, n, n)
        let zeta = try gpu.texture(.depth32Float_stencil8, n, n, usage: [.renderTarget])
        let solid = try gpu.render(
            fragment: "solid_fragment", format: .rgba8Unorm, depth: .depth32Float_stencil8,
            stencil: .depth32Float_stencil8)
        let shadow = try gpu.render(
            fragment: "shadow_stencil_fragment", format: .rgba8Unorm, depth: .depth32Float_stencil8,
            stencil: .depth32Float_stencil8)
        let writeDepth = MTLDepthStencilDescriptor()
        writeDepth.depthCompareFunction = .always
        writeDepth.isDepthWriteEnabled = true
        let tested = MTLDepthStencilDescriptor()
        tested.depthCompareFunction = .less
        tested.isDepthWriteEnabled = true
        let face = MTLStencilDescriptor()
        face.stencilCompareFunction = .always
        face.depthFailureOperation = .incrementClamp
        face.depthStencilPassOperation = .replace
        tested.frontFaceStencil = face
        tested.backFaceStencil = face
        guard let first = gpu.device.makeDepthStencilState(descriptor: writeDepth),
            let second = gpu.device.makeDepthStencilState(descriptor: tested)
        else { throw TestFailure("no depth state") }
        func quad(_ e: MTLRenderCommandEncoder, x1: Float, z: Float, _ rgba: SIMD4<Float>) {
            var corners: [SIMD4<Float>] = [[-1, -1, z, 1], [x1, -1, z, 1], [-1, 1, z, 1], [x1, 1, z, 1]]
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            var c = rgba
            e.setFragmentBytes(&c, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            var offset: UInt32 = 0
            e.setFragmentBytes(&offset, length: 4, index: 1)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        }
        let path = try await gpu.run { commands in
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
            p.stencilAttachment.clearStencil = 16
            p.stencilAttachment.storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: p)!
            e.setRenderPipelineState(solid)
            e.setDepthStencilState(first)
            quad(e, x1: 0, z: 0.5, [0, 0, 1, 1])
            e.setRenderPipelineState(shadow)
            e.setDepthStencilState(second)
            quad(e, x1: 1, z: 0.75, [0, 1, 0, 1])
            e.endEncoding()
        }.path
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
        let s = stencilBytes.contents().bindMemory(to: UInt8.self, capacity: n * n)
        var colourWrong = 0, stencilWrong = 0
        for y in 0..<n {
            for x in 0..<n {
                let left = x < n / 2
                let at = 4 * (x + n * y)
                colourWrong += Array(pixels[at..<at + 4]) == (left ? [0, 0, 255, 255] : [0, 255, 0, 255]) ? 0 : 1
                stencilWrong += s[x + n * y] == (left ? 17 : UInt8(x)) ? 0 : 1
            }
        }
        var metrics = Metrics()
        metrics.set("path", path)
        metrics.set("colour_wrong", colourWrong)
        metrics.set("stencil_wrong", stencilWrong)
        metrics.set("stencil_x2_x20", "\(s[2])/\(s[20])")
        var failures: [String] = []
        if path != "raw" { failures.append("the pass went off the raw path (\(path))") }
        if colourWrong != 0 { failures.append("\(colourWrong) pixels wrong") }
        if stencilWrong != 0 { failures.append("\(stencilWrong) stencil texels wrong") }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}
