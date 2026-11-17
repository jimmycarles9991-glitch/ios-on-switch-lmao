import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A slice-choosing state whose fragment function returns [[stencil]], in a layered pass of two layers over a Stencil8
/// array: the raw path runs the shader-side stencil against a shadow of every layer (a 2D array, each fragment at its
/// layer). Draw 1, red, writes stencil 5 into layer 0 and 9 into layer 1 (replace); draw 2, cyan, its output 9, passes
/// where the stencil equals it: layer 1 alone. An M4 Max stores 255/0/0/255 in layer 0 and 0/255/255/255 in layer 1 at
/// every pixel. The base build fails the command buffer (raw:draw.layer_dropped: the translation had no layered form of
/// the shader-side stencil).
struct LayeredShaderStencilTest: HWTest {
    let id = "gpu.raw.layered_shader_stencil"
    let name = "A layered pass's shader-side stencil reads a shadow of every layer on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct LayeredOut { float4 p [[position]]; uint layer [[render_target_array_index]]; };
        vertex LayeredOut layered_stencil_vertex(uint v [[vertex_id]], uint i [[instance_id]]) {
            const float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
            LayeredOut o;
            o.p = float4(p[v], 0.5, 1);
            o.layer = i;
            return o;
        }
        struct StencilOut { float4 colour [[color(0)]]; uint stencil [[stencil]]; };
        fragment StencilOut layered_stencil_fragment(LayeredOut in [[stage_in]], constant float4 &c [[buffer(0)]]) {
            return {c, c.b > 0 ? 9u : (in.layer == 0 ? 5u : 9u)};
        }
        """

    static let size = 8
    static let expected: [[UInt8]] = [[255, 0, 0, 255], [0, 255, 255, 255]]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let device = gpu.device
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "layered_stencil_vertex")
        descriptor.fragmentFunction = library.makeFunction(name: "layered_stencil_fragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.stencilAttachmentPixelFormat = .stencil8
        descriptor.inputPrimitiveTopology = .triangle
        let state = try await device.makeRenderPipelineState(descriptor: descriptor)
        func stencilState(_ compare: MTLCompareFunction, _ pass: MTLStencilOperation) throws -> MTLDepthStencilState {
            let face = MTLStencilDescriptor()
            face.stencilCompareFunction = compare
            face.depthStencilPassOperation = pass
            let d = MTLDepthStencilDescriptor()
            d.frontFaceStencil = face
            d.backFaceStencil = face
            guard let s = device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no stencil state") }
            return s
        }
        let write = try stencilState(.always, .replace), equal = try stencilState(.equal, .keep)
        let colors = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: Self.size, height: Self.size, mipmapped: false)
        colors.textureType = .type2DArray
        colors.arrayLength = 2
        colors.usage = [.renderTarget, .shaderRead]
        colors.storageMode = .private
        let stencils = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .stencil8, width: Self.size, height: Self.size, mipmapped: false)
        stencils.textureType = .type2DArray
        stencils.arrayLength = 2
        stencils.usage = [.renderTarget]
        stencils.storageMode = .private
        guard let color = device.makeTexture(descriptor: colors), let stencil = device.makeTexture(descriptor: stencils)
        else { throw TestFailure("no layered targets") }
        let timing = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = color
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            pass.stencilAttachment.texture = stencil
            pass.stencilAttachment.loadAction = .clear
            pass.stencilAttachment.clearStencil = 0
            pass.stencilAttachment.storeAction = .dontCare
            pass.renderTargetArrayLength = 2
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            for (stencilTest, colour) in [(write, SIMD4<Float>(1, 0, 0, 1)), (equal, SIMD4<Float>(0, 1, 1, 1))] {
                var colour = colour
                encoder.setDepthStencilState(stencilTest)
                encoder.setFragmentBytes(&colour, length: 16, index: 0)
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3, instanceCount: 2)
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
