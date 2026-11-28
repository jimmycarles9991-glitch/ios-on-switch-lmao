import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Kernels whose buffers are bound at addresses off 16 bytes, by element: floats at 4, float2s at 8, packed float3s at
/// 4. The descriptor starts 16 below and the record's residual word holds the rest; a slot whose record names none is
/// one its program never addresses. Every element written is the source's doubled plus one, every byte around it
/// untouched, and every dispatch raw.
struct KernelWindowScene: GPUScene {
    let id = "gpu.gm20b.kernel_window"
    let name = "Kernel buffers bound at addresses off 16 bytes"

    static let count = 37
    static let sentinel: UInt8 = 0xA5
    static let cases: [(function: String, floats: Int, offset: Int)] = [
        ("window_float", 1, 4), ("window_float2", 2, 8), ("window_packed3", 3, 4),
    ]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        var paths: [String] = []
        for (function, floats, offset) in Self.cases {
            let pipeline = try gpu.compute(function)
            let bytes = Self.count * floats * 4
            let length = offset + bytes + 16
            let source = try gpu.buffer(length)
            let destination = try gpu.buffer(length)
            let values = (0 ..< Self.count * floats).map { Float($0 % 97) * 0.25 - 3.0 }
            (source.contents() + offset).copyMemory(from: values, byteCount: bytes)
            memset(destination.contents(), Int32(Self.sentinel), length)
            let timing = try await gpu.run { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(pipeline)
                encoder.setBuffer(source, offset: offset, index: 0)
                encoder.setBuffer(destination, offset: offset, index: 1)
                encoder.dispatchThreads(
                    MTLSize(width: Self.count, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: Self.count, height: 1, depth: 1)
                )
                encoder.endEncoding()
            }
            paths.append(timing.path)
            let out = destination.contents().assumingMemoryBound(to: UInt8.self)
            var wrong = 0
            for at in 0 ..< length where at < offset || at >= offset + bytes {
                if out[at] != Self.sentinel { wrong += 1 }
            }
            let written = (destination.contents() + offset).assumingMemoryBound(to: Float.self)
            for i in 0 ..< values.count where written[i] != values[i] * 2 + 1 { wrong += 1 }
            result.metrics.set("\(function)_wrong", wrong)
            result.expect(wrong == 0, "\(function) at \(offset): \(wrong) values or bytes around them wrong")
        }
        result.metrics.set("paths", paths.joined(separator: ","))
        if GM20BFacade.kind(of: gpu.device) == "gm20b" {
            result.expect(paths.allSatisfy { $0 == "gm20b" }, "the dispatches took \(paths), not raw")
        }
        return result
    }
}
