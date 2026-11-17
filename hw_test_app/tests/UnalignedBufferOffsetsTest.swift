import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Buffers bound off the descriptor base alignment on the raw path (enc.ubo): twenty vertex uniform buffers each 4
/// bytes past a multiple of 256 plus a slot's own step, eight fragment storage buffers each 4 to 12 bytes past a
/// multiple of 16, each holding one float of 1/128 where it is bound and 1 everywhere else: 29 buffer entries with the
/// corners', within the 32 a record holds (raw_pack.h kPackEntries). Every residual is in its record word, the vertex
/// stage's last ones in its argument table; one lost reads 1 and the channel saturates. Metal's sums are 20/128 (red
/// 40) and 8/128 (green 16); the pass must stay raw.
struct UnalignedBufferOffsetsTest: HWTest {
    let id = "gpu.raw.unaligned_buffer_offsets"
    let name = "Uniform and storage buffers bound off their base alignment read through their residual words, raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static func uniformOffset(_ slot: Int) -> Int { 256 * slot + 4 * (slot + 1) }
    static func storageOffset(_ slot: Int) -> Int { 16 * slot + 4 * (1 + slot % 3) }

    static func values(_ gpu: GPU, bytes: Int, at offsets: [Int]) throws -> MTLBuffer {
        var values = [Float](repeating: 1, count: bytes / 4)
        for offset in offsets { values[offset / 4] = 1.0 / 128 }
        return try gpu.buffer(values)
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let uniforms = try Self.values(gpu, bytes: 8192, at: (1...20).map(Self.uniformOffset))
        let storage = try Self.values(gpu, bytes: 1024, at: (0...7).map(Self.storageOffset))
        let state = try gpu.render(vertex: "residual_vertex", fragment: "residual_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 4, 4)
        let drawn = try await gpu.run { commands in
            let p = MTLRenderPassDescriptor()
            p.colorAttachments[0].texture = target
            p.colorAttachments[0].loadAction = .clear
            p.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: p)!
            e.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
            for slot in 1...20 { e.setVertexBuffer(uniforms, offset: Self.uniformOffset(slot), index: slot) }
            for slot in 0...7 { e.setFragmentBuffer(storage, offset: Self.storageOffset(slot), index: slot) }
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
        }
        let pixel = Array(try await gpu.read(target, bytesPerPixel: 4).pixel(2, 2, width: 4))
        var metrics = Metrics()
        metrics.set("path", drawn.path)
        metrics.set("pixel", pixel.map(String.init).joined(separator: ","))
        var failures: [String] = []
        if drawn.path != "raw" { failures.append("the pass went off the raw path: \(drawn.path)") }
        if abs(Int(pixel[0]) - 40) > 1 || abs(Int(pixel[1]) - 16) > 1 {
            failures.append("red \(pixel[0]) green \(pixel[1]), Metal 40 and 16")
        }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
