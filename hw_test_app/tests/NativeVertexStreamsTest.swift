import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A state of two fixed-function vertex streams on the raw path: positions in buffer 1 (8 bytes apart), colours in buffer
/// 2 (32 bytes apart, junk between them). One draw of two quads, the left half red and the right half blue, each vertex
/// its own colour from stream 1. A second stream left unbound, bound at stream 0's address or read at the wrong stride
/// draws black, red or junk where the expected colour is.
struct NativeVertexStreamsTest: NativeObjectTest {
    let id = "metal.native.vertex_streams"
    let name = "A state of two vertex buffers reads each from its own bind on the raw path"

    static let size = 32
    static let colourStride = 32

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct In { float2 position [[attribute(0)]]; float4 colour [[attribute(1)]]; };
        struct Out { float4 position [[position]]; float4 colour [[flat]]; };
        vertex Out two_stream_vertex(In in [[stage_in]]) { return {float4(in.position, 0, 1), in.colour}; }
        fragment float4 two_stream_fragment(Out in [[stage_in]]) { return in.colour; }
        """

    /// Each quad's two triangles over [x0, x1] of the whole height, and its colour
    static let quads: [(x0: Float, x1: Float, colour: SIMD4<Float>)] = [
        (-1, 0, SIMD4(1, 0, 0, 1)), (0, 1, SIMD4(0, 0, 1, 1)),
    ]

    static var positions: [Float] {
        quads.flatMap { q in [q.x0, -1, q.x1, -1, q.x0, 1, q.x0, 1, q.x1, -1, q.x1, 1] }
    }

    static var colours: [Float] {
        let floats = colourStride / 4
        var out: [Float] = []
        for q in quads {
            for _ in 0..<6 {
                out += [q.colour.x, q.colour.y, q.colour.z, q.colour.w]
                out += [Float](repeating: 1e6, count: floats - 4)
            }
        }
        return out
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        let vertices = MTLVertexDescriptor()
        vertices.attributes[0].format = .float2
        vertices.attributes[0].bufferIndex = 1
        vertices.layouts[1].stride = 8
        vertices.attributes[1].format = .float4
        vertices.attributes[1].bufferIndex = 2
        vertices.layouts[2].stride = Self.colourStride
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "two_stream_vertex")
        descriptor.fragmentFunction = library.makeFunction(name: "two_stream_fragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.vertexDescriptor = vertices
        let pipeline = try await device.makeRenderPipelineState(descriptor: descriptor)
        work.objects.append(pipeline)
        let target = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
        let positions = try gpu.buffer(Self.positions)
        let colours = try gpu.buffer(Self.colours)
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 1, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setVertexBuffer(positions, offset: 0, index: 1)
            encoder.setVertexBuffer(colours, offset: 0, index: 2)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 6 * Self.quads.count)
            encoder.endEncoding()
        }
        work.paths.append(timing.path)
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let wrong = (0..<Self.size * Self.size).filter {
            let x = $0 % Self.size
            let expected: [UInt8] = x < Self.size / 2 ? [255, 0, 0, 255] : [0, 0, 255, 255]
            return Array(pixels.pixel(x, $0 / Self.size, width: Self.size)) != expected
        }.count
        metrics.set("wrong_pixels", wrong)
        if wrong != 0 { work.failures.append("\(wrong) of \(Self.size * Self.size) pixels wrong") }
        return work
    }
}
