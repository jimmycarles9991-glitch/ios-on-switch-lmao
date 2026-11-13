import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// metal.native.argument_encoder's draws of functions whose bind offsets m2v places past their stage's push window, in the
/// stage's argument table the raw path fills (raw_spill_table.h): an argument buffer beside fourteen fragment buffers,
/// beside thirteen vertex buffers, or both in one draw, each a float. Every slot is bound 4 bytes past a multiple of 16,
/// so each residual is in its word: the root's, which m2v's plan names where the probe's codes are shared, or the
/// table's.
extension NativeArgumentEncoderTest {
    static let spillStride = 528

    enum SpillStages {
        case fragment, vertex, both
    }

    static func spillOffset(_ slot: Int) -> Int {
        slot * spillStride + 4
    }

    /// One float, `value`, at each slot's offset and twice it 8 bytes past slot 30's, `elsewhere` everywhere else: a
    /// residual lost reads `elsewhere`
    static func spillValues(_ gpu: GPU, _ value: Float, _ elsewhere: Float) throws -> MTLBuffer {
        var values = [Float](repeating: elsewhere, count: 32 * spillStride / 4)
        for slot in 1..<32 { values[spillOffset(slot) / 4] = value }
        values[(30 * spillStride + 8) / 4] = value * 2
        return try gpu.buffer(values)
    }

    /// Where two draws of spilling stages went, the second after slot 14 moved to a float twice the others (its table
    /// uploaded again, nothing else), and whether each read every buffer at its own offset: argument_spill_fragment,
    /// spill_vertex, or spill_vertex with spill_tint_fragment
    func drawSpillThroughArguments(_ gpu: GPU, _ stages: SpillStages) async throws -> String {
        guard let encoder = gpu.device.makeArgumentEncoder(arguments: [Self.argument(.pointer, 0)]) else {
            return "no_encoder"
        }
        let colour = try gpu.buffer([SIMD4<Float>(0, 0, 1, 1)]), arguments = try gpu.buffer(16)
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.setBuffer(colour, offset: 0, index: 0)
        let values = try Self.spillValues(gpu, 1.0 / 32, 1)
        let vertex = stages != .fragment, fragment = stages != .vertex
        let pipeline = try gpu.render(
            vertex: vertex ? "spill_vertex" : "quad_vertex",
            fragment: [.fragment: "argument_spill_fragment", .vertex: "tint_fragment", .both: "spill_tint_fragment"][
                stages]!,
            format: .rgba8Unorm)
        let targets = try (0..<2).map { _ in try gpu.texture(.rgba8Unorm, 2, 1) }
        let drawn = try await gpu.run { commands in
            for (k, target) in targets.enumerated() {
                let render = commands.makeRenderCommandEncoder(descriptor: Self.pass(target))!
                render.useResource(colour, usage: .read, stages: [vertex ? .vertex : [], fragment ? .fragment : []])
                render.setRenderPipelineState(pipeline)
                var corners = GPU.fullScreen
                render.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
                if vertex {
                    for slot in 2...14 { render.setVertexBuffer(values, offset: Self.spillOffset(slot), index: slot) }
                    render.setVertexBuffer(arguments, offset: 0, index: 1)
                }
                if fragment {
                    for slot in 1...14 { render.setFragmentBuffer(values, offset: Self.spillOffset(slot), index: slot) }
                    render.setFragmentBuffer(arguments, offset: 0, index: 0)
                }
                render.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                if k == 1 {
                    if vertex { render.setVertexBufferOffset(30 * Self.spillStride + 8, index: 14) }
                    if fragment { render.setFragmentBufferOffset(30 * Self.spillStride + 8, index: 14) }
                    render.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                render.endEncoding()
            }
        }
        // each spilling stage's channel: 13 (green) or 14 (red) thirty-seconds, then one more
        var exact = true
        for k in 0..<2 {
            let pixels = try await gpu.read(targets[k], bytesPerPixel: 4)
            let red: UInt8 = fragment ? (k == 0 ? 112 : 120) : 0, green: UInt8 = vertex ? (k == 0 ? 104 : 112) : 0
            let want: [UInt8] = [red, green, 255, 255]
            exact = exact && (0..<2).allSatisfy { Array(pixels.pixel($0, 0, width: 2)) == want }
        }
        return "\(drawn.path)/\(exact ? "exact" : "wrong")"
    }
}
