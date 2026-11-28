import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Extended-range textures past one-level 2D, raw's with a stand-in of their shape (xr_parts.h): a BGR10_XR cube's
/// faces and a BGRA10_XR 3D texture's planes and level read as half (each the half nearest (code - 384) / 510, as this
/// Mac's Metal reads them: m2vf-shapes2 probe/hwref/xrshapes.swift), an RGB10A8_2P_XR10 texture's colour words through
/// the CPU (probe/hwref/xr550cpu.swift), and a multisampled, a mipmapped and a 1D one made. Each went to m2v before.
struct TextureShapesXRTest: HWTest {
    let id = "gpu.gm20b.texture_shapes_xr"
    let name = "Extended-range arrays, cubes, 3D, 1D, mipmapped and multisampled textures are raw's own"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        if let skip = ShapeArms.skip(gpu) { return skip }
        let arms = ShapeArms(gpu)
        func half(_ code: Int) -> [UInt8] {
            let bits = Float16(Double(code - 384) / 510).bitPattern
            return [UInt8(bits & 0xff), UInt8(bits >> 8)]
        }
        let one: [UInt8] = [0x00, 0x3c]
        let cube = arms.make(.bgr10_xr, .typeCube, 4, 4, storage: .shared, usage: .shaderRead)
        arms.route("xr_cube", cube, expect: "gm20b")
        if let cube {
            var expect: [UInt8] = []
            for face in 0 ..< 6 {
                let r = 384 + 100 * face, g = 1023 - 37 * face, b = 50 + 9 * face
                let word = UInt32(0xc000_0000) | UInt32(r) << 20 | UInt32(g) << 10 | UInt32(b)
                cube.replace(
                    region: MTLRegionMake2D(0, 0, 4, 4), mipmapLevel: 0, slice: face,
                    withBytes: [UInt32](repeating: word, count: 16), bytesPerRow: 16, bytesPerImage: 0
                )
                expect += half(r) + half(g) + half(b) + one
            }
            let read = try await arms.drawn(cube, fragment: "shapes_cube_face_half", width: 6, format: .rgba16Float)
            arms.off("xr_cube", read, expect)
        }
        let volume = arms.make(.bgra10_xr, .type3D, 4, 4, depth: 2, levels: 2, storage: .shared, usage: .shaderRead)
        arms.route("xr_3d", volume, expect: "gm20b")
        if let volume {
            var expect: [UInt8] = []
            let planes = [(0, 0, 900, 384, 0, 894), (0, 1, 384, 700, 1023, 640), (1, 0, 0, 0, 500, 384)]
            for (level, plane, r, g, b, a) in planes {
                let side = level == 0 ? 4 : 2
                let texel: [UInt16] = [UInt16(b << 6), UInt16(g << 6), UInt16(r << 6), UInt16(a << 6)]
                volume.replace(
                    region: MTLRegionMake3D(0, 0, plane, side, side, 1), mipmapLevel: level, slice: 0,
                    withBytes: Array([[UInt16]](repeating: texel, count: side * side).joined()),
                    bytesPerRow: 8 * side, bytesPerImage: 8 * side * side
                )
                expect += half(r) + half(g) + half(b) + half(a)
            }
            let read = try await arms.drawn(volume, fragment: "shapes_volume_half", width: 3, format: .rgba16Float)
            arms.off("xr_3d", read, expect)
        }
        let words = arms.make(MTLPixelFormat(rawValue: 550)!, .type2D, 4, 4, storage: .shared, usage: .shaderRead)
        arms.route("xr_planes_shared", words, expect: "gm20b")
        if let words {
            arms.off(
                "xr_planes_shared", try arms.cpuRoundTrip(words, width: 4, height: 4, bytesPerRow: 16, rows: 4), []
            )
        }
        arms.route(
            "xr_ms", arms.make(.bgr10_xr, .type2DMultisample, 8, 8, samples: 4, usage: .renderTarget), expect: "gm20b"
        )
        arms.route("xr_chain", arms.make(.bgra10_xr, .type2D, 16, 16, levels: 3, usage: .shaderRead), expect: "gm20b")
        arms.route("xr_1d", arms.make(.bgr10_xr, .type1D, 16, 1, usage: .shaderRead), expect: "gm20b")
        return arms.outcome()
    }
}
