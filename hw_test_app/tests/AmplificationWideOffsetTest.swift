import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// View mappings whose offset is past 16 bits on 24B119's A13 (21H461's A10X answers amplification NO): an M4 Max draws
/// an amplification whose layer or viewport index is at or past the pass's layers or the encoder's viewports into
/// slice or viewport 0, 70000 as it does 4. A full-target quad at count 2 into two layers through two viewports (the
/// halves), amplification 0 offset by 70000 layers in one arm and 70000 viewports in the other, amplification 1 mapped
/// to layer 1 and viewport 1: layer 0 holds red in its left half alone, layer 1 green in its right half alone, every
/// command buffer raw (raw refused such a mapping, whose macro word holds 16 bits, and m2v has no amplification).
struct AmplificationWideOffsetTest: NativeObjectTest {
    let id = "gpu.raw.amplification_wide_offset"
    let name = "Vertex amplification view mapping offsets past 16 bits draw into slice and viewport 0 on the raw path"

    static let size = 32

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let amplifies = device.supportsVertexAmplificationCount(2)
        metrics.set("amplification", amplifies ? "yes" : "no")
        guard amplifies else { return work }
        let library = try await device.makeLibrary(source: NativeAmplificationMappingsTest.source, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "mapped_quad")
        descriptor.fragmentFunction = library.makeFunction(name: "mapped_color")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.inputPrimitiveTopology = .triangle
        descriptor.maxVertexAmplificationCount = 2
        let pipeline = try await device.makeRenderPipelineState(descriptor: descriptor)
        work.objects.append(pipeline)
        let half = Double(Self.size / 2)
        typealias Mapping = MTLVertexAmplificationViewMapping
        let mapped = Mapping(viewportArrayIndexOffset: 1, renderTargetArrayIndexOffset: 1)
        for (label, wide) in [
            ("layer", Mapping(viewportArrayIndexOffset: 0, renderTargetArrayIndexOffset: 70_000)),
            ("viewport", Mapping(viewportArrayIndexOffset: 70_000, renderTargetArrayIndexOffset: 0)),
        ] {
            let t = try Layering.array(gpu, Self.size, layers: 2)
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
                var mappings = [wide, mapped]
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
            metrics.set("\(label)_wrong", wrong)
            if wrong != 0 { work.failures.append("\(label): \(wrong) texels off Metal's slice and viewport 0") }
        }
        return work
    }
}
