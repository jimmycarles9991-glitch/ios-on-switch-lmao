import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// RG8Unorm_sRGB through its RGBA8Unorm_sRGB stand-in (raw_srgb_pair.h): a shared 4x1 one's red and green bytes read by
/// a draw as an RGBA8Unorm_sRGB texture of the same bytes (blue 0, alpha 255) reads them, every component exact (this
/// Mac's Metal: all 65,536 texels, m2vf-shapes2 probe/hwref/rg8srgb.swift); a 2D array and a mipmapped one made. Each
/// went to m2v before (no format row).
struct TextureShapesSrgbPairTest: HWTest {
    let id = "gpu.raw.texture_shapes_srgb_pair"
    let name = "RG8Unorm_sRGB textures are raw's own and sample as Metal decodes them"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        if let skip = ShapeArms.skip(gpu) { return skip }
        let arms = ShapeArms(gpu)
        let pair = arms.make(.rg8Unorm_srgb, .type2D, 4, 1, storage: .shared, usage: .shaderRead)
        arms.route("srgb_pair", pair, expect: "raw")
        let reference = arms.make(.rgba8Unorm_srgb, .type2D, 4, 1, storage: .shared, usage: .shaderRead)
        if let pair, let reference {
            let bytes: [UInt8] = [0, 0, 64, 128, 188, 255, 255, 30]
            pair.replace(region: MTLRegionMake2D(0, 0, 4, 1), mipmapLevel: 0, withBytes: bytes, bytesPerRow: 8)
            let wide = (0..<4).flatMap { i -> [UInt8] in [bytes[2 * i], bytes[2 * i + 1], 0, 255] }
            reference.replace(region: MTLRegionMake2D(0, 0, 4, 1), mipmapLevel: 0, withBytes: wide, bytesPerRow: 16)
            let read = { (texture: MTLTexture) in
                try await arms.drawn(texture, fragment: "view_read_2d", width: 4, format: .rgba32Float)
            }
            arms.off("srgb_pair", try await read(pair), try await read(reference))
        }
        arms.route(
            "srgb_pair_array", arms.make(.rg8Unorm_srgb, .type2DArray, 8, 8, array: 3, usage: .shaderRead),
            expect: "raw")
        arms.route(
            "srgb_pair_chain", arms.make(.rg8Unorm_srgb, .type2D, 16, 16, levels: 3, usage: .shaderRead), expect: "raw")
        return arms.outcome()
    }
}
