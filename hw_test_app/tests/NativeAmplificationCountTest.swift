import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// [[amplification_count]] per draw where the device answers amplification (24B119's A13; 21H461's A10X answers NO):
/// one state of maxVertexAmplificationCount 2 whose vertex function splits the target into as many columns as the
/// draw's count and puts each amplification in its own, coloured by the count and the id. At count 2 the left half is red
/// and the right green; at count 1 the whole target is blue. Each count drawn directly and through indirect arguments,
/// one amplify call a draw, every command buffer raw.
struct NativeAmplificationCountTest: NativeObjectTest {
    let id = "metal.native.amplification_count"
    let name = "[[amplification_count]] is each draw's count on the raw path"

    static let size = 32

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct Out { float4 position [[position]]; float4 color; };
        vertex Out counted_quad(uint v [[vertex_id]], ushort a [[amplification_id]], ushort n [[amplification_count]]) {
            const float2 corner = float2(v & 1, v >> 1);
            const float width = 2.0 / n;
            Out o;
            o.position = float4(-1 + (corner.x + a) * width, corner.y * 2 - 1, 0, 1);
            o.color = n == 1 ? float4(0, 0, 1, 1) : a ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1);
            return o;
        }
        fragment float4 counted_color(Out in [[stage_in]]) { return in.color; }
        """

    static func expected(_ x: Int, count: Int) -> [UInt8] {
        count == 1 ? [0, 0, 255, 255] : x < size / 2 ? [255, 0, 0, 255] : [0, 255, 0, 255]
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let amplifies = device.supportsVertexAmplificationCount(2)
        metrics.set("amplification", amplifies ? "yes" : "no")
        guard amplifies else { return work }
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "counted_quad")
        descriptor.fragmentFunction = library.makeFunction(name: "counted_color")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.maxVertexAmplificationCount = 2
        let pipeline = try await device.makeRenderPipelineState(descriptor: descriptor)
        work.objects.append(pipeline)
        let arguments = try gpu.buffer([UInt32(4), 1, 0, 0])
        for (count, indirect) in [(2, false), (1, false), (2, true), (1, true)] {
            let target = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
            let timing = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                encoder.setVertexAmplificationCount(count, viewMappings: nil)
                if indirect {
                    encoder.drawPrimitives(type: .triangleStrip, indirectBuffer: arguments, indirectBufferOffset: 0)
                } else {
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            work.paths.append(timing.path)
            let pixels = try await gpu.read(target, bytesPerPixel: 4)
            let wrong = (0..<Self.size * Self.size).filter { i in
                let x = i % Self.size
                return Array(pixels.pixel(x, i / Self.size, width: Self.size)) != Self.expected(x, count: count)
            }.count
            let name = indirect ? "indirect_count\(count)" : "count\(count)"
            metrics.set("\(name)_wrong_pixels", wrong)
            if wrong != 0 { work.failures.append("\(name): \(wrong) pixels wrong") }
        }
        return work
    }
}
