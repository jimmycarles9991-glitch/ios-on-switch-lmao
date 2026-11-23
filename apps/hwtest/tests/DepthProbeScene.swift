import Foundation
import HWTestGPU
import Metal

/// The scene of the M4 Max probes gpu.gm20b.depth_format_mismatch and gpu.gm20b.hidden_depth hold the driver to
/// (c38-audit m2v-draws/depth_probe.mm, hidden_probe.mm): an 8x8 RGBA8 target cleared black, each draw one full-screen
/// triangle at depth `z` in a solid colour.
struct DepthProbeScene {
    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct Out { float4 p [[position]]; };
    vertex Out v(uint id [[vertex_id]], constant float &z [[buffer(0)]]) {
        const float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
        Out o; o.p = float4(p[id], z, 1); return o;
    }
    fragment float4 f(constant float4 &c [[buffer(0)]]) { return c; }
    """
    static let side = 8
    static let red: [UInt8] = [255, 0, 0, 255]
    static let green: [UInt8] = [0, 255, 0, 255]
    static let black: [UInt8] = [0, 0, 0, 255]

    let library: MTLLibrary
    let gpu: GPU

    init(_ gpu: GPU) async throws {
        self.gpu = gpu
        library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
    }

    func state(depth: MTLPixelFormat = .invalid, stencil: MTLPixelFormat = .invalid) throws -> MTLRenderPipelineState {
        let d = MTLRenderPipelineDescriptor()
        d.vertexFunction = library.makeFunction(name: "v")
        d.fragmentFunction = library.makeFunction(name: "f")
        d.colorAttachments[0].pixelFormat = .rgba8Unorm
        d.depthAttachmentPixelFormat = depth
        d.stencilAttachmentPixelFormat = stencil
        return try gpu.device.makeRenderPipelineState(descriptor: d)
    }

    func colour() throws -> MTLTexture { try gpu.texture(.rgba8Unorm, Self.side, Self.side, usage: [.renderTarget]) }

    static func pass(_ colour: MTLTexture) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = colour
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        return pass
    }

    static func draw(_ encoder: MTLRenderCommandEncoder, z: Float, _ colour: SIMD4<Float>) {
        var z = z
        var colour = colour
        encoder.setVertexBytes(&z, length: 4, index: 0)
        encoder.setFragmentBytes(&colour, length: 16, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    }

    /// The target's pixels that are not `want`
    func off(_ colour: MTLTexture, _ want: [UInt8]) async throws -> Int {
        let pixels = try await gpu.read(colour, bytesPerPixel: 4)
        return (0 ..< Self.side * Self.side).filter { Array(pixels[4 * $0 ..< 4 * $0 + 4]) != want }.count
    }
}
