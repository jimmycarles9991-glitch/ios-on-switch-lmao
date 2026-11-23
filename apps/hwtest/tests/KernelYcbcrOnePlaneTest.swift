import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A compute kernel reading a '2vuy' surface as YCBCR8_422_1P (501), whose kernel the shader compiler once refused
/// (compute.nokernel): through read() and through a linear sample a quarter texel off each centre, every texel equal to
/// the same kernel's read of YCBCR8_422_2P (502) over a '422v' surface of the same codes (gpu.gm20b.ycbcr_one_plane's
/// surfaces). Fields: read_off= and linear_off= texels that differ, paths=, failed_cbs=.
struct KernelYcbcrOnePlaneTest: HWTest {
    let id = "gpu.gm20b.kernel_ycbcr_one_plane"
    let name = "A kernel reads and samples a one-plane 4:2:2 texture as the two-plane one, on raw"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        guard let (one, two) = YcbcrOnePlaneTest.textures(gpu, YcbcrOnePlaneTest.arms[0]) else {
            return .fail("no 501 texture over '2vuy' or 502 over '422v'")
        }
        let linear = try gpu.compute("sample_texture_linear")
        func sampled(_ texture: MTLTexture) async throws -> [SIMD4<Float>] {
            let count = texture.width * texture.height
            let out = try gpu.buffer(count * 16)
            try await gpu.run { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(linear)
                encoder.setTexture(texture, index: 0)
                encoder.setBuffer(out, offset: 0, index: 0)
                encoder.dispatchThreads(
                    MTLSize(width: texture.width, height: texture.height, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
                )
                encoder.endEncoding()
            }
            return Array(
                UnsafeBufferPointer(
                    start: out.contents().bindMemory(to: SIMD4<Float>.self, capacity: count), count: count
                )
            )
        }
        let readOff = zip(try await gpu.texels(one), try await gpu.texels(two)).filter { $0 != $1 }.count
        let linearOff = zip(try await sampled(one), try await sampled(two)).filter { $0 != $1 }.count
        var metrics = Metrics()
        metrics.set("read_off", readOff)
        metrics.set("linear_off", linearOff)
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        return .judged(
            [
                ("read() of 501 is 502's", readOff == 0), ("a linear sample of 501 is 502's", linearOff == 0),
                ("every command buffer raw", gpu.paths.offRaw.isEmpty),
                FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics),
            ], metrics, proof: gpu.proof
        )
    }
}
