import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A slice-choosing state whose fetching fragment function may discard, depth-tested with writes, in a layered pass of two
/// layers: the raw path runs the depth test in its program against a stand-in of the pass's layers (a 2D array, each
/// fragment at its layer). Three draws: z 0.5 into both layers, z 0.75 into both (fails the test), z 0.25 into layer 1.
/// Each writes its colour plus half the colour it fetches. An M4 Max stores 128/0/0/255 in layer 0 and 64/0/128/255 in
/// layer 1 at every pixel. The base build fails the command buffer (raw:draw.layer_dropped: the translation had no layered
/// form of the in-shader depth test).
struct LayeredShaderTestsTest: HWTest {
    let id = "gpu.raw.layered_shader_tests"
    let name = "A layered pass's in-shader depth test reads a stand-in of every layer on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct LayeredOut { float4 p [[position]]; uint layer [[render_target_array_index]]; };
        vertex LayeredOut layered_tests_vertex(uint v [[vertex_id]], uint i [[instance_id]],
                                               constant float &z [[buffer(0)]]) {
            const float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
            LayeredOut o;
            o.p = float4(p[v], z, 1);
            o.layer = i;
            return o;
        }
        fragment float4 layered_tests_fragment(LayeredOut in [[stage_in]], float4 previous [[color(0)]],
                                               constant float4 &c [[buffer(0)]]) {
            if (previous.r < 0) discard_fragment();
            return float4(c.rgb + previous.rgb * 0.5, 1);
        }
        """

    static let size = 8
    static let expected: [[UInt8]] = [[128, 0, 0, 255], [64, 0, 128, 255]]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let device = gpu.device
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "layered_tests_vertex")
        descriptor.fragmentFunction = library.makeFunction(name: "layered_tests_fragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.depthAttachmentPixelFormat = .depth32Float
        descriptor.inputPrimitiveTopology = .triangle
        let state = try await device.makeRenderPipelineState(descriptor: descriptor)
        let tests = MTLDepthStencilDescriptor()
        tests.depthCompareFunction = .less
        tests.isDepthWriteEnabled = true
        guard let depthState = device.makeDepthStencilState(descriptor: tests) else {
            throw TestFailure("no depth state")
        }
        let colors = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: Self.size, height: Self.size, mipmapped: false)
        colors.textureType = .type2DArray
        colors.arrayLength = 2
        colors.usage = [.renderTarget, .shaderRead]
        colors.storageMode = .private
        let depths = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .depth32Float, width: Self.size, height: Self.size, mipmapped: false)
        depths.textureType = .type2DArray
        depths.arrayLength = 2
        depths.usage = [.renderTarget]
        depths.storageMode = .private
        guard let color = device.makeTexture(descriptor: colors), let depth = device.makeTexture(descriptor: depths)
        else { throw TestFailure("no layered targets") }
        let timing = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = color
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = .dontCare
            pass.renderTargetArrayLength = 2
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            encoder.setDepthStencilState(depthState)
            let draws: [(Float, SIMD4<Float>, Int, Int)] = [
                (0.5, SIMD4(0.5, 0, 0, 0), 2, 0), (0.75, SIMD4(0, 0.5, 0, 0), 2, 0), (0.25, SIMD4(0, 0, 0.5, 0), 1, 1),
            ]
            for (z, c, instances, base) in draws {
                var z = z
                var c = c
                encoder.setVertexBytes(&z, length: 4, index: 0)
                encoder.setFragmentBytes(&c, length: 16, index: 0)
                encoder.drawPrimitives(
                    type: .triangle, vertexStart: 0, vertexCount: 3, instanceCount: instances, baseInstance: base)
            }
            encoder.endEncoding()
        }
        var metrics = Metrics()
        metrics.set("path", String(timing.path.prefix(160)))
        var wrong = 0
        if timing.path.hasPrefix("error:") {
            wrong = -1
        } else {
            for layer in 0..<2 {
                let bytes = try await gpu.read(color, bytesPerPixel: 4, slice: layer)
                metrics.set(
                    "layer\(layer)", bytes.pixel(3, 3, width: Self.size).map { String($0) }.joined(separator: "/"))
                wrong +=
                    (0..<Self.size * Self.size).filter {
                        Array(bytes.pixel($0 % Self.size, $0 / Self.size, width: Self.size)) != Self.expected[layer]
                    }.count
            }
        }
        metrics.set("wrong_pixels", wrong)
        return timing.path == "raw" && wrong == 0
            ? .pass(metrics, proof: gpu.proof) : .fail("path \(timing.path.prefix(80)), \(wrong) pixels wrong", metrics)
    }
}
