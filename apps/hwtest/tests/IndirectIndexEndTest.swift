import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// An indexed indirect draw whose index buffer offset is at or past the buffer's end, which Metal's validation takes:
/// the driver reads index 0 at every position (the zero page, as NVK binds an empty range). Three points at base
/// vertex 5 over a buffer of 7s, each point at x = its vertex id writing id + 1: only x5 lit. The control arm's offset
/// holds one index (7): only x12. The past_page arm draws 20000 points, past the 64 KiB zero page's 16384 indices, where
/// the index fetch's past-limit zero (3D-B11) answers: still only x5. An M4 Max reads whatever memory follows the buffer
/// instead (undefined).
struct IndirectIndexEndTest: HWTest {
    let id = "gpu.gm20b.indirect_index_end"
    let name = "Indexed indirect draws at or past their index buffer's end read index 0 on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct IndexEndPoint { float4 p [[position]]; float s [[point_size]]; uint id [[flat]]; };
    vertex IndexEndPoint index_end_vertex(uint vid [[vertex_id]]) {
        IndexEndPoint o;
        o.p = float4((float(vid) + 0.5) / 16.0 * 2.0 - 1.0, 0, 0.5, 1);
        o.s = 1;
        o.id = vid;
        return o;
    }
    fragment uint index_end_fragment(IndexEndPoint in [[stage_in]]) { return in.id + 1; }
    """

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "index_end_vertex")
        descriptor.fragmentFunction = library.makeFunction(name: "index_end_fragment")
        descriptor.colorAttachments[0].pixelFormat = .r32Uint
        descriptor.inputPrimitiveTopology = .point
        let state = try await gpu.device.makeRenderPipelineState(descriptor: descriptor)
        let indices = try gpu.buffer([UInt32](repeating: 7, count: 1024))
        let arms: [(label: String, offset: Int, count: UInt32, lit: String)] = [
            ("at_end", 4096, 3, "x5=6"),
            ("past_end", 8192, 3, "x5=6"),
            ("past_page", 4096, 20000, "x5=6"),
            ("control", 4092, 1, "x12=13"),
        ]
        var metrics = Metrics()
        var wrong: [String] = []
        for arm in arms {
            let arguments = try gpu.buffer([arm.count, 1, 0, 5, 0])
            let target = try gpu.texture(.r32Uint, 16, 1)
            let timing = try await gpu.run(tolerateError: true) { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(state)
                encoder.drawIndexedPrimitives(
                    type: .point, indexType: .uint32, indexBuffer: indices, indexBufferOffset: arm.offset,
                    indirectBuffer: arguments, indirectBufferOffset: 0
                )
                encoder.endEncoding()
            }
            var lit = "-"
            if !timing.path.hasPrefix("error:") {
                let bytes = try await gpu.read(target, bytesPerPixel: 4)
                let ids = (0 ..< 16).map { bytes.loaded(as: UInt32.self, at: $0) }
                lit = ids.enumerated().filter { $0.element != 0 }.map { "x\($0.offset)=\($0.element)" }
                    .joined(separator: ",")
            }
            metrics.set("\(arm.label)_path", String(timing.path.prefix(120)))
            metrics.set("\(arm.label)_lit", lit)
            if timing.path != "gm20b" || lit != arm.lit { wrong.append(arm.label) }
        }
        return wrong.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(wrong.joined(separator: ","), metrics)
    }
}
