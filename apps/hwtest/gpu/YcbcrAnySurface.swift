import Foundation
import HWTestCore
import IOSurface
import Metal

/// YCBCR8_420_2P (500) 32x16 textures over surfaces of other formats and layouts, each surface's byte i
/// SurfaceReads.pattern(i), read by a kernel and compared texel for texel with a 500 over a '420v' (or '420f') whose
/// planes hold, in rows of their own, the bytes an M4 Max reads (gpu.gm20b.ycbcr_any_surface)
public enum YcbcrAnySurface {
    typealias P = SurfaceReads.Plane

    public static let scenes = ["x420", "xf20", "bgra", "y3p8", "rows40"]

    /// A '420v' (`full`: '420f') 32 x `height`, rows 32, chroma at 512, luma row y the source's bytes from luma(y),
    /// chroma row j from chroma(j) (nil: zeros)
    static func reference(
        _ source: [UInt8], height: Int, full: Bool, luma: (Int) -> Int, chroma: ((Int) -> Int)?
    ) -> IOSurfaceRef? {
        let at = { (i: Int) in i < source.count ? source[i] : 0 }
        let planes = [P(at: 0, 32, height, bpe: 1, rows: 32), P(at: 512, 16, height / 2, bpe: 2, rows: 32)]
        return SurfaceReads.surface(full ? "420f" : "420v", 32, height, planes: planes, allocation: 1024) { i in
            if i < 512 { return i / 32 < height ? at(luma(i / 32) + i % 32) : 0 }
            let row = (i - 512) / 32
            return row < height / 2 ? chroma.map { at($0(row) + (i - 512) % 32) } ?? 0 : 0
        }
    }

    static func surface(_ scene: String) -> (IOSurfaceRef?, plane: Int) {
        switch scene {
        case "x420", "xf20":
            return (
                SurfaceReads.surface(scene, 32, 16, planes: [
                    P(at: 0, 32, 16, bpe: 2, rows: 64),
                    P(at: 1024, 16, 8, bpe: 4, rows: 64),
                ], allocation: 2048),
                0
            )
        case "bgra":
            return (
                SurfaceReads.surface("BGRA", 32, 16, planes: [P(at: 0, 32, 16, bpe: 4, rows: 128)], allocation: 2048),
                0
            )
        case "y3p8":
            let planes = [0, 512, 768].map { P(at: $0, 32, 8, bpe: 1, rows: 32) }
            return (SurfaceReads.surface("y3p8", 32, 8, planes: planes, allocation: 1024), 1)
        default:
            return (
                SurfaceReads.surface("420v", 32, 16, planes: [
                    P(at: 0, 32, 16, bpe: 1, rows: 40),
                    P(at: 640, 16, 8, bpe: 2, rows: 40),
                ], allocation: 1024),
                0
            )
        }
    }

    static func layout(_ scene: String, _ source: [UInt8]) -> IOSurfaceRef? {
        switch scene {
        case "x420", "xf20":
            return reference(source, height: 16, full: scene == "xf20", luma: { 64 * $0 }, chroma: { 1024 + 64 * $0 })
        case "bgra": return reference(source, height: 16, full: false, luma: { 128 * $0 }, chroma: nil)
        case "y3p8": return reference(
                source,
                height: 8,
                full: false,
                luma: { 512 + 32 * $0 },
                chroma: { 768 + 32 * $0 }
            )
        default: return reference(source, height: 16, full: false, luma: { 32 * $0 }, chroma: { 640 + 32 * $0 })
        }
    }

    /// Texels not the reference's; nil where Metal (or the facade) made no texture over the scene's surface
    public static func off(_ gpu: GPU, _ scene: String) async throws -> Int? {
        let (made, plane) = surface(scene)
        guard let source = made else { throw TestFailure("no '\(scene)' surface") }
        let height = scene == "y3p8" ? 8 : 16
        guard let texture = SurfaceReads.texture(gpu, 500, 32, height, over: source, plane: plane) else { return nil }
        guard let mirror = layout(scene, SurfaceReads.bytes(source)),
              let alike = SurfaceReads.texture(gpu, 500, 32, height, over: mirror)
        else { throw TestFailure("no reference for '\(scene)'") }
        let got = try await SurfaceReads.read(gpu, texture), want = try await SurfaceReads.read(gpu, alike)
        return got.indices.filter { got[$0] != want[$0] }.count
    }

    /// 3x, 3z: a Clear (0.25, 0.5, 0.75, 1) into a 500 over the rows-40 '420v' and over the one-plane 'BGRA' changes bytes
    /// [0, 512) and [640, 896), and the first 32 of each 128-byte row (115 each) alone: the bytes changed elsewhere
    public static func storeOutside(_ gpu: GPU) async throws -> Int {
        var outside = 0
        for scene in ["rows40", "bgra"] {
            let (made, _) = surface(scene)
            guard let surface = made,
                  let texture = SurfaceReads.texture(
                      gpu,
                      500,
                      32,
                      16,
                      over: surface,
                      usage: [.shaderRead, .renderTarget]
                  )
            else { throw TestFailure("no 500 target over '\(scene)'") }
            let before = SurfaceReads.bytes(surface)
            let timing = try await gpu.run(tolerateError: true) { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = texture
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
            guard !timing.path.hasPrefix("error:") else { throw TestFailure("the clear failed: \(timing.facadePath)") }
            let after = SurfaceReads.bytes(surface)
            let inside = { (i: Int) in scene == "bgra" ? i % 128 < 32 : i < 512 || (640 ..< 896).contains(i) }
            outside += after.indices.filter { !inside($0) && after[$0] != before[$0] }.count
            if scene == "bgra" { outside += after.indices.filter { inside($0) && after[$0] != 115 }.count }
        }
        return outside
    }
}
