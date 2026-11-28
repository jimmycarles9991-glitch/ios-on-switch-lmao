import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestX1
import Metal

/// Texture buffers (MTLTextureTypeTextureBuffer) over a buffer at the answered minimumTextureBufferAlignment and at twice
/// it, read texel by texel by a kernel (shaders/HWTest.metal read_texture_buffer): each under a texel buffer header of its
/// own on the driver (gm20b::Device::createTexelBuffer), where raw made none and m2v refused them (boot 22
/// gpu.x1.linear_align: "no texture buffer at offset 64"). <offset>_path= each read's path, <offset>_wrong= its channels
/// off the bytes, alignment= the answer.
struct TextureBufferTest: HWTest {
    let id = "gpu.gm20b.texture_buffer"
    let name = "Texture buffers at the answered alignment, read by a kernel on the driver"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let alignment = gpu.device.minimumTextureBufferAlignment(for: .rgba8Unorm)
        var metrics = Metrics()
        metrics.set("alignment", alignment)
        var checks: [(String, Bool)] = []
        for offset in [alignment, 2 * alignment] {
            let (path, wrong) = try await X1MemoryProbes.textureBufferRead(gpu, offset: offset)
            metrics.set("\(offset)_path", path)
            metrics.set("\(offset)_wrong", wrong)
            checks.append(("texture buffer at \(offset) read exact", wrong == 0))
            if GM20BFacade.kind(of: gpu.device) == "gm20b" {
                checks.append(("texture buffer at \(offset) on raw", path == "gm20b"))
            }
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
