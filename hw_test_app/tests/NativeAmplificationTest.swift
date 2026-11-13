import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Vertex amplification where the device answers it (24B119's A13; 21H461's A10X answers NO and refuses the state): a
/// state of maxVertexAmplificationCount 2 whose vertex function moves its quad right by [[amplification_id]] halves and
/// colours it by the id. A draw at count 2 paints the left half red and the right half green, one amplification after
/// the other; the same state at count 1 paints the left half alone. Each count again as an execute of an indirect command
/// buffer whose one command draws the quad, inheriting the state: the execute draws its range once per amplification.
/// Every command buffer stays raw.
struct NativeAmplificationTest: NativeObjectTest {
    let id = "metal.native.amplification"
    let name = "Vertex amplification drawn one amplification at a time on the raw path"

    static let size = 32

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct Out { float4 position [[position]]; float4 color; };
        vertex Out amplified_quad(uint v [[vertex_id]], ushort a [[amplification_id]]) {
            const float2 corner = float2(v & 1, v >> 1);
            Out o;
            o.position = float4(-1 + corner.x + a, corner.y * 2 - 1, 0, 1);
            o.color = a ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1);
            return o;
        }
        fragment float4 amplified_color(Out in [[stage_in]]) { return in.color; }
        """

    /// Pixels other than red in the left half, and green in the right one where `both`, black where not
    static func wrong(_ pixels: [UInt8], both: Bool) -> Int {
        (0..<size * size).filter { i in
            let left = i % size < size / 2
            let want: [UInt8] = left ? [255, 0, 0, 255] : both ? [0, 255, 0, 255] : [0, 0, 0, 255]
            return Array(pixels.pixel(i % size, i / size, width: size)) != want
        }.count
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "amplified_quad")
        descriptor.fragmentFunction = library.makeFunction(name: "amplified_color")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.maxVertexAmplificationCount = 2
        descriptor.supportIndirectCommandBuffers = true
        let amplifies = device.supportsVertexAmplificationCount(2)
        metrics.set("amplification", amplifies ? "yes" : "no")
        guard amplifies else {
            if (try? await device.makeRenderPipelineState(descriptor: descriptor)) != nil {
                work.failures.append("a state of amplification 2 was made where the device answers it NO")
            }
            return work
        }
        let pipeline = try await device.makeRenderPipelineState(descriptor: descriptor)
        work.objects.append(pipeline)
        let inherits = MTLIndirectCommandBufferDescriptor()
        inherits.commandTypes = [.draw]
        inherits.inheritPipelineState = true
        inherits.inheritBuffers = true
        guard let quad = device.makeIndirectCommandBuffer(descriptor: inherits, maxCommandCount: 1, options: []) else {
            throw TestFailure("makeIndirectCommandBuffer returned nil for an inherited draw")
        }
        work.objects.append(quad)
        quad.indirectRenderCommandAt(0).drawPrimitives(
            .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: 1, baseInstance: 0)
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
                    encoder.executeCommandsInBuffer(quad, range: 0..<1)
                } else {
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            work.paths.append(timing.path)
            let wrong = Self.wrong(try await gpu.read(target, bytesPerPixel: 4), both: count == 2)
            let name = indirect ? "icb_count\(count)" : "count\(count)"
            metrics.set("\(name)_wrong_pixels", wrong)
            if wrong != 0 { work.failures.append("\(name): \(wrong) pixels wrong") }
        }
        return work
    }
}
