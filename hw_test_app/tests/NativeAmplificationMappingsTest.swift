import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// setVertexAmplificationCount:viewMappings: of render target and viewport offsets on 24B119's A13 (21H461's A10X answers
/// amplification NO): a full-target quad drawn at count 2 into a layered pass of two layers through two viewports, the
/// left and right halves, amplification 0 mapped to layer 0 and viewport 0, amplification 1 to layer 1 and viewport 1,
/// coloured by its id. Layer 0 holds red in its left half alone and layer 1 green in its right half alone, exact texel by
/// texel, every command buffer raw.
struct NativeAmplificationMappingsTest: NativeObjectTest {
    let id = "metal.native.amplification_mappings"
    let name = "Vertex amplification's view mappings choose each amplification's layer and viewport on the raw path"

    static let size = 32

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct Out { float4 position [[position]]; float4 color; };
        vertex Out mapped_quad(uint v [[vertex_id]], ushort a [[amplification_id]]) {
            Out o;
            o.position = float4(float(v & 1) * 2 - 1, float(v >> 1) * 2 - 1, 0, 1);
            o.color = a ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1);
            return o;
        }
        fragment float4 mapped_color(Out in [[stage_in]]) { return in.color; }
        """

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let amplifies = device.supportsVertexAmplificationCount(2)
        metrics.set("amplification", amplifies ? "yes" : "no")
        guard amplifies else { return work }
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "mapped_quad")
        descriptor.fragmentFunction = library.makeFunction(name: "mapped_color")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.inputPrimitiveTopology = .triangle
        descriptor.maxVertexAmplificationCount = 2
        let pipeline = try await device.makeRenderPipelineState(descriptor: descriptor)
        let t = try Layering.array(gpu, Self.size, layers: 2)
        work.objects.append(pipeline)
        work.objects.append(t)
        let half = Double(Self.size / 2)
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.renderTargetArrayLength = 2
            pass.colorAttachments[0].texture = t
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setViewports([
                MTLViewport(originX: 0, originY: 0, width: half, height: Double(Self.size), znear: 0, zfar: 1),
                MTLViewport(originX: half, originY: 0, width: half, height: Double(Self.size), znear: 0, zfar: 1),
            ])
            var mappings = [
                MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: 0, renderTargetArrayIndexOffset: 0),
                MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: 1, renderTargetArrayIndexOffset: 1),
            ]
            encoder.setVertexAmplificationCount(2, viewMappings: &mappings)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        work.paths.append(timing.path)
        let wrong = try await Layering.wrong(gpu, t, paths: &work.paths) { layer, x, _ in
            let left = x < Self.size / 2
            if layer == 0 && left { return [255, 0, 0, 255] }
            if layer == 1 && !left { return [0, 255, 0, 255] }
            return [0, 0, 0, 255]
        }
        metrics.set("wrong_texels", wrong)
        if wrong != 0 { work.failures.append("\(wrong) texels outside their amplification's layer and viewport") }
        return work
    }
}
