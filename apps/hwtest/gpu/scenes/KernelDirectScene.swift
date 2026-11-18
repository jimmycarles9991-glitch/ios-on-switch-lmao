import Foundation
import HWTestCore
import Metal

/// A kernel no pack holds (this app's multiply_add, at a threadgroup shape of its own): on the raw facade its record is made
/// at its first dispatch (drivers/TegraGM20BMetal/raw/runtime/RawComplete.h), so that dispatch and the next both run on raw,
/// never m2v, and each result is exact.
struct KernelDirectScene: GPUScene {
    let id = "gpu.raw.kernel_direct"
    let name = "A kernel the pack lacks, made on raw at its first dispatch"

    static let threads = 96
    static let count = 96 * 512

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let a = (0..<Self.count).map { Float($0 % 977) * 0.5 }
        let b = (0..<Self.count).map { Float($0 % 13) - 6 }
        let pipeline = try gpu.compute("multiply_add")
        var paths: [String] = []
        for dispatch in 0..<2 {
            let out = try gpu.buffer(Self.count * 4)
            let counter = try gpu.buffer([UInt32(0)])
            let timing = try await gpu.run { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(pipeline)
                encoder.setBuffer(try gpu.buffer(a), offset: 0, index: 0)
                encoder.setBuffer(try gpu.buffer(b), offset: 0, index: 1)
                encoder.setBuffer(out, offset: 0, index: 2)
                encoder.setBuffer(counter, offset: 0, index: 3)
                encoder.dispatchThreadgroups(
                    MTLSize(width: Self.count / Self.threads, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: Self.threads, height: 1, depth: 1))
                encoder.endEncoding()
            }
            paths.append(timing.path)
            let results = out.contents().bindMemory(to: Float.self, capacity: Self.count)
            // Every product and sum is a small multiple of 0.5, exact in float32, so fma and a*b+c agree bit for bit.
            let wrong = (0..<Self.count).filter { results[$0] != a[$0] * b[$0] + Float($0 & 1023) }
            result.expect(
                wrong.isEmpty, "dispatch \(dispatch): \(wrong.count) wrong, the first at \(wrong.first ?? -1)")
            result.expect(
                counter.contents().load(as: UInt32.self) == UInt32(Self.count),
                "dispatch \(dispatch): not one atomic increment per thread")
        }
        result.metrics.set("paths", paths.joined(separator: ","))
        if RawFacade.kind(of: gpu.device) == "raw" {
            result.expect(paths == ["raw", "raw"], "the first and second dispatches took \(paths), not raw")
        }
        return result
    }
}
