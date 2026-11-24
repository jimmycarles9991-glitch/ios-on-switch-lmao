import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A post-tessellation vertex function that reads a texture through an argument buffer, on the driver: its program
/// reads the texture's handle through set 1, which the draw binds at the tessellation program's constant buffer (raw
/// refused such draws, and m2v has no patch draw). One quad patch at factor 1 covers the target, each vertex taking the
/// texture's one texel as its colour. Reported, not judged: the same patch drawn by a post-tessellation function that
/// only places it (no argument buffer, no texture) and a constant magenta fragment (constant_drawn, of 64; -1 where it
/// could not run): every pixel magenta says patches make their fragments and this test's argument read is what misses;
/// none says patches make no fragments at all.
struct TessellationArgumentTextureTest: HWTest {
    let id = "gpu.gm20b.tess_argument_texture"
    let name = "A post-tessellation vertex function reads a texture through an argument buffer on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let n = 8
    static let texel: [UInt8] = [40, 160, 220, 255]

    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct ControlPoint { float4 position [[attribute(0)]]; };
    struct QuadPatch { patch_control_point<ControlPoint> points; };
    struct Arguments { texture2d<float> colour [[id(0)]]; };
    struct Out { float4 position [[position]]; float4 colour; };
    [[patch(quad, 4)]] vertex Out tess_argument_post(QuadPatch patch [[stage_in]], float2 uv [[position_in_patch]],
                                                     constant Arguments &arguments [[buffer(1)]]) {
        const float4 low = mix(patch.points[0].position, patch.points[1].position, uv.x);
        const float4 high = mix(patch.points[3].position, patch.points[2].position, uv.x);
        Out out;
        out.position = mix(low, high, uv.y);
        out.colour = arguments.colour.read(uint2(0, 0));
        return out;
    }
    fragment float4 tess_argument_colour(Out in [[stage_in]]) { return in.colour; }
    [[patch(quad, 4)]] vertex float4 tess_plain_post(QuadPatch patch [[stage_in]], float2 uv [[position_in_patch]]) {
        const float4 low = mix(patch.points[0].position, patch.points[1].position, uv.x);
        const float4 high = mix(patch.points[3].position, patch.points[2].position, uv.x);
        return mix(low, high, uv.y);
    }
    fragment float4 tess_plain_magenta() { return float4(1, 0, 1, 1); }
    """
    static let magenta: [UInt8] = [255, 0, 255, 255]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let n = Self.n
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        guard let post = library.makeFunction(name: "tess_argument_post") else { throw TestFailure("no post function") }
        let vertices = MTLVertexDescriptor()
        vertices.attributes[0].format = .float4
        vertices.attributes[0].bufferIndex = 0
        vertices.layouts[0].stride = MemoryLayout<SIMD4<Float>>.stride
        vertices.layouts[0].stepFunction = .perPatchControlPoint
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = post
        descriptor.fragmentFunction = library.makeFunction(name: "tess_argument_colour")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.vertexDescriptor = vertices
        descriptor.tessellationPartitionMode = .integer
        descriptor.maxTessellationFactor = 16
        let state = try await gpu.device.makeRenderPipelineState(descriptor: descriptor)
        descriptor.vertexFunction = library.makeFunction(name: "tess_plain_post")
        descriptor.fragmentFunction = library.makeFunction(name: "tess_plain_magenta")
        let constant = try? await gpu.device.makeRenderPipelineState(descriptor: descriptor)
        let texture = try gpu.texture(.rgba8Unorm, 1, 1, usage: [.shaderRead], storage: .shared)
        texture.replace(region: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0, withBytes: Self.texel, bytesPerRow: 4)
        let encoder = post.makeArgumentEncoder(bufferIndex: 1)
        let arguments = try gpu.buffer(max(encoder.encodedLength, 16))
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.setTexture(texture, index: 0)
        let points = try gpu.buffer([
            SIMD4<Float>(-1, -1, 0, 1), SIMD4<Float>(1, -1, 0, 1), SIMD4<Float>(1, 1, 0, 1), SIMD4<Float>(-1, 1, 0, 1),
        ])
        let factors = try gpu.buffer([Float16](repeating: 1, count: 6))
        let target = try gpu.texture(.rgba8Unorm, n, n)
        func draw(_ state: MTLRenderPipelineState) async throws -> String {
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let render = commands.makeRenderCommandEncoder(descriptor: pass)!
                render.setRenderPipelineState(state)
                render.useResource(texture, usage: .read, stages: .vertex)
                render.setVertexBuffer(points, offset: 0, index: 0)
                render.setVertexBuffer(arguments, offset: 0, index: 1)
                render.setTessellationFactorBuffer(factors, offset: 0, instanceStride: 0)
                render.drawPatches(
                    numberOfPatchControlPoints: 4, patchStart: 0, patchCount: 1, patchIndexBuffer: nil,
                    patchIndexBufferOffset: 0, instanceCount: 1, baseInstance: 0
                )
                render.endEncoding()
            }.path
        }
        let path = try await draw(state)
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        var wrong = 0
        for y in 0 ..< n {
            for x in 0 ..< n where Array(pixels.pixel(x, y, width: n)) != Self.texel { wrong += 1 }
        }
        var metrics = Metrics()
        metrics.set("path", path)
        metrics.set("pixels_wrong", wrong)
        metrics.set("middle", Array(pixels.pixel(n / 2, n / 2, width: n)).map(String.init).joined(separator: "/"))
        if let constant, let constantPath = try? await draw(constant),
           let constantPixels = try? await gpu.read(target, bytesPerPixel: 4) {
            metrics.set("constant_path", constantPath)
            metrics.set(
                "constant_drawn",
                (0 ..< n * n).filter { Array(constantPixels.pixel($0 % n, $0 / n, width: n)) == Self.magenta }.count
            )
        } else {
            metrics.set("constant_drawn", -1)
        }
        var failures: [String] = []
        if path != "gm20b" { failures.append("path") }
        if wrong > 0 { failures.append("pixels") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: ","), metrics)
    }
}
