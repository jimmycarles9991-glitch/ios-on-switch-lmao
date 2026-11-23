import CoreVideo
import Foundation
import HWTestCore
import IOSurface
import Metal

/// Copies between textures held in planes, as an M4 Max moves them (gpu.gm20b.planes_copies_open). A source with no plane
/// 1 (504 over 'v410', 501 over '2vuy', 562 over '2vuy') leaves a two-plane destination's plane 1 alone. Between a 550
/// made from a descriptor (32x8) and one over a 'b3a8' 32x8 (words rows 128 at 0, alpha rows 32 at 1024, one page), an
/// 8x2 copy from (5, 0) to (3, 0) moves the words texel for texel and the alpha bytes as between two surfaces: a byte a
/// texel at the WORD rows (128), so its second row is alpha row 4 of a 32-wide texture on either side.
public enum PlaneCopiesOpen {
    public struct Result: Sendable {
        public var made = true
        public var path = "-"
        /// Destination bytes (a surface) or texels (a descriptor-made 550) off Apple's
        public var off = -1
    }

    /// One-plane source into two planes: 8x4 from (2, 2) to (1, 1) of 16x8 textures, plane 0 in the destination's unit
    /// (4 for 550, 1 for 500), plane 1 as it was
    public static let onePlaneSources: [(key: String, from: (String, UInt, Int), to: (String, UInt))] = [
        ("v504_b3a8", ("v410", 504, 4), ("b3a8", 550)), ("vuy_420v", ("2vuy", 501, 2), ("420v", 500)),
        ("v562_b3a8", ("2vuy", 562, 2), ("b3a8", 550)),
    ]

    static func fourcc(_ s: String) -> OSType { s.utf8.reduce(OSType(0)) { $0 << 8 | OSType($1) } }

    static func seeded(_ surface: IOSurfaceRef, _ seed: UInt64) -> [UInt8] {
        let fill = PlaneCopies.seeded(seed)
        RowsUnder16.edit(surface) { base in for i in 0 ..< IOSurfaceGetAllocSize(surface) { base[i] = fill(i) } }
        return SurfaceReads.bytes(surface)
    }

    /// A one-plane surface 16x8 of `unit`-byte rows, packed
    static func onePlane(_ code: String, unit: Int) -> IOSurfaceRef? {
        IOSurfaceCreate(
            [
                kIOSurfaceWidth: 16, kIOSurfaceHeight: 8, kIOSurfacePixelFormat: fourcc(code),
                kIOSurfaceBytesPerRow: 16 * unit, kIOSurfaceBytesPerElement: 4, kIOSurfaceElementWidth: 4 / unit,
                kIOSurfaceAllocSize: 16 * unit * 8,
            ] as [CFString: Any] as CFDictionary
        )
    }

    /// A two-plane destination 16x8: 'b3a8' words rows 64 at 0, alpha rows 64 at 512; '420v' luma rows 16 at 0,
    /// chroma rows 16 at 128
    static func twoPlanes(_ code: String) -> IOSurfaceRef? {
        typealias P = SurfaceReads.Plane
        let planes =
            code == "b3a8"
                ? [P(at: 0, 16, 8, bpe: 4, rows: 64), P(at: 512, 16, 8, bpe: 1, rows: 64)]
                : [P(at: 0, 16, 8, bpe: 1, rows: 16), P(at: 128, 8, 4, bpe: 2, rows: 16)]
        return SurfaceReads.surface(code, 16, 8, planes: planes, allocation: 16384)
    }

    public static func onePlaneSource(_ gpu: GPU, _ c: (key: String, from: (String, UInt, Int), to: (String, UInt)))
        async throws -> Result
    {
        guard let from = onePlane(c.from.0, unit: c.from.2), let into = twoPlanes(c.to.0) else {
            throw TestFailure("\(c.key): no surfaces")
        }
        let source = seeded(from, 11), before = seeded(into, 29)
        guard let s = SurfaceReads.texture(gpu, c.from.1, 16, 8, over: from),
              let d = SurfaceReads.texture(gpu, c.to.1, 16, 8, over: into)
        else { return Result(made: false) }
        let path = try await RowsUnder16.blitPath(gpu) {
            $0.copy(
                from: s, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 2, y: 2, z: 0),
                sourceSize: MTLSize(width: 8, height: 4, depth: 1), to: d, destinationSlice: 0, destinationLevel: 0,
                destinationOrigin: MTLOrigin(x: 1, y: 1, z: 0)
            )
        }
        let u = c.to.1 == 550 ? 4 : 1, rows = c.to.1 == 550 ? 64 : 16, sourceRows = 16 * c.from.2
        var want = before
        for k in 0 ..< 4 {
            for i in 0 ..< 8 * u { want[(1 + k) * rows + u + i] = source[(2 + k) * sourceRows + 2 * u + i] }
        }
        let got = SurfaceReads.bytes(into)
        return Result(path: path, off: zip(got, want).filter { $0 != $1 }.count)
    }

    /// Texel (x, y) of the descriptor-made side as forms_open_xr_store writes it: codes red 128 + 3x + 40y, green 300
    /// + 7x + 11y, blue 600 + x + 50y, alpha (5 + 7x + 29y) & 255
    static func coded(_ x: Int, _ y: Int) -> (word: UInt32, alpha: UInt8) {
        (UInt32(128 + 3 * x + 40 * y) << 20 | UInt32(300 + 7 * x + 11 * y) << 10 | UInt32(600 + x + 50 * y),
         UInt8((5 + 7 * x + 29 * y) & 255))
    }

    static let surfacePlanes = [
        SurfaceReads.Plane(at: 0, 32, 8, bpe: 4, rows: 128), SurfaceReads.Plane(at: 1024, 32, 8, bpe: 1, rows: 32),
    ]
    static let region = (
        from: MTLOrigin(x: 5, y: 0, z: 0), size: MTLSize(width: 8, height: 2, depth: 1), to: MTLOrigin(x: 3, y: 0, z: 0)
    )

    /// The alpha a copy moves to linear alpha byte `dy * 128 + 3 + j` from the source's linear alpha byte
    /// `dy * 128 + 5 + j` (`dy` < 2, `j` < 8), each in its 32-wide texture's (x, y)
    static func alphaMoves() -> [(to: (Int, Int), from: (Int, Int))] {
        (0 ..< 2).flatMap { r in (0 ..< 8).map { j in ((3 + j, 4 * r), (5 + j, 4 * r)) } }
    }

    /// A descriptor-made 550 32x8, every texel `coded`, copied into a 550 over a seeded 'b3a8'
    public static func descriptorIntoSurface(_ gpu: GPU) async throws -> Result {
        guard let made = try await descriptorMade(gpu) else { return Result(made: false) }
        guard let into = SurfaceReads.surface("b3a8", 32, 8, planes: surfacePlanes, allocation: 16384) else {
            throw TestFailure("no 'b3a8' 32x8")
        }
        let before = seeded(into, 41)
        guard let d = SurfaceReads.texture(gpu, 550, 32, 8, over: into) else { return Result(made: false) }
        let path = try await copy(gpu, made, d)
        var want = before
        for y in 0 ..<
            2 { for j in 0 ..< 8 { RowsUnder16.put(coded(5 + j, y).word, &want, at: y * 128 + 4 * (3 + j)) } }
        for m in alphaMoves() { want[1024 + m.to.1 * 32 + m.to.0] = coded(m.from.0, m.from.1).alpha }
        let got = SurfaceReads.bytes(into)
        return Result(path: path, off: zip(got, want).filter { $0 != $1 }.count)
    }

    /// A 550 over a seeded 'b3a8' copied into a descriptor-made 550 32x8 whose texels are `coded`, judged by a kernel's
    /// read of every texel (half a code either way)
    public static func surfaceIntoDescriptor(_ gpu: GPU) async throws -> Result {
        guard let made = try await descriptorMade(gpu) else { return Result(made: false) }
        guard let from = SurfaceReads.surface("b3a8", 32, 8, planes: surfacePlanes, allocation: 16384) else {
            throw TestFailure("no 'b3a8' 32x8")
        }
        let source = seeded(from, 43)
        guard let s = SurfaceReads.texture(gpu, 550, 32, 8, over: from) else { return Result(made: false) }
        let path = try await copy(gpu, s, made)
        var words = (0 ..< 256).map { coded($0 % 32, $0 / 32).word }
        var alphas = (0 ..< 256).map { coded($0 % 32, $0 / 32).alpha }
        for y in 0 ..<
            2 { for j in 0 ..< 8 { words[32 * y + 3 + j] = SurfaceReads.word(source, y * 128 + 4 * (5 + j)) } }
        for m in alphaMoves() { alphas[32 * m.to.1 + m.to.0] = source[1024 + 32 * m.from.1 + m.from.0] }
        let got = try await SurfaceReads.read(gpu, made)
        let off = SurfaceReads.off(got, tolerance: 0.5 / 510) {
            SurfaceReads.xr(words[$0], alpha: Float(alphas[$0]) / 255)
        }
        return Result(path: path, off: off)
    }

    static func descriptorMade(_ gpu: GPU) async throws -> MTLTexture? {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: 550)!, width: 32, height: 8, mipmapped: false
        )
        descriptor.usage = [.shaderRead, .shaderWrite]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor) else { return nil }
        let path = try await RowsUnder16.dispatch(gpu, "forms_open_xr_store", texture)
        guard !path.hasPrefix("error") else { throw TestFailure("the descriptor-made 550's store failed: \(path)") }
        return texture
    }

    static func copy(_ gpu: GPU, _ s: MTLTexture, _ d: MTLTexture) async throws -> String {
        try await RowsUnder16.blitPath(gpu) {
            $0.copy(
                from: s, sourceSlice: 0, sourceLevel: 0, sourceOrigin: region.from, sourceSize: region.size, to: d,
                destinationSlice: 0, destinationLevel: 0, destinationOrigin: region.to
            )
        }
    }
}
