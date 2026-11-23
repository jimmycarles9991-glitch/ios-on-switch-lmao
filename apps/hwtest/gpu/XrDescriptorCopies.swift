import Foundation
import HWTestCore
import IOSurface
import Metal

/// Copies between RGB10A8_2P_XR10 (550) textures a descriptor made (D) and one over a 'b3a8' 32x8 (S: words rows 128 at
/// 0, alpha rows 32 at 1024, one page, every byte seeded), as an M4 Max moves them with validation off: each side's
/// words (4 bytes a texel) and alpha (1 a texel) are two planes, byte b of region row j's texel x at
/// row * Pb + unit * x + b. S's Pb is 128 for both planes from 0 and 1024; D's is (4W) & ~15 (16 under 4 texels) over
/// its own rows of words ru16(4W) bytes apart and of alpha ru16(W) apart. Bytes past a row's texels or past the texture
/// read 0 and keep no write a texel read shows; a D under 4 texels wide gives its row 0 alone as a source. Where two
/// destination bytes are written by one copy, either value may land (a race on an M4 Max).
public enum XrDescriptorCopies {
    public enum Side: Sendable {
        case surface
        case descriptor(width: Int, height: Int)
    }

    public struct Case: Sendable {
        public let key: String
        let from: Side, to: Side
        let origin: (x: Int, y: Int), size: (w: Int, h: Int), at: (x: Int, y: Int)

        init(_ key: String, _ from: Side, _ origin: (Int, Int), _ size: (Int, Int), _ to: Side, _ at: (Int, Int)) {
            (self.key, self.from, self.to, self.origin, self.size, self.at) = (key, from, to, origin, size, at)
        }
    }

    static func d(_ w: Int, _ h: Int) -> Side { .descriptor(width: w, height: h) }

    /// The seven copies of the t13 spec and one of each further family of the 287-case probe (a 33-wide descriptor both
    /// ways, a descriptor under 4 texels as a destination, a sub-rectangle between two descriptors)
    public static let cases: [Case] = [
        Case("s_d17", .surface, (3, 1), (13, 3), d(17, 4), (2, 0)),
        Case("s_d16", .surface, (0, 0), (16, 8), d(16, 8), (0, 0)),
        Case("s_d2", .surface, (5, 2), (2, 6), d(2, 8), (0, 1)),
        Case("d17_s", d(17, 4), (1, 1), (16, 3), .surface, (4, 2)),
        Case("d16_s", d(16, 8), (0, 0), (16, 8), .surface, (0, 0)),
        Case("d2_s", d(2, 8), (0, 0), (2, 8), .surface, (7, 0)),
        Case("d17_d33", d(17, 4), (0, 0), (16, 4), d(33, 5), (1, 1)),
        Case("s_d33", .surface, (0, 0), (32, 5), d(33, 5), (0, 0)),
        Case("d33_s", d(33, 5), (1, 0), (32, 5), .surface, (0, 3)),
        Case("s_d2_rows", .surface, (4, 3), (2, 5), d(2, 8), (0, 0)),
        Case("d16_d17", d(16, 8), (2, 1), (8, 4), d(17, 4), (5, 0)),
    ]

    public struct Result: Sendable {
        public var path = "-"
        /// Destination bytes (S) or texel channels (D) off the rule, and of the source
        public var off = -1
        public var sourceOff = -1
        /// Destination bytes or channels the rule changes
        public var moved = 0
    }

    /// One plane of one side: where byte `at` of the plane lies, a surface's byte or a descriptor's texel and byte
    struct Plane {
        let surface: Bool, base: Int, bound: Int, pitch: Int, unit: Int
        let rowBytes: Int, stored: Int, rows: Int, rowZeroOnly: Bool

        init(_ side: Side, words: Bool, source: Bool) {
            unit = words ? 4 : 1
            switch side {
            case .surface:
                (surface, base, pitch) = (true, words ? 0 : 1024, 128)
                (bound, rowBytes, stored, rows, rowZeroOnly) = (16384 - base, 0, 0, 0, false)
            case .descriptor(let width, let height):
                (surface, base, bound) = (false, 0, 0)
                pitch = 4 * width < 16 ? 16 : (4 * width) & ~15
                rowBytes = unit * width
                stored = (rowBytes + 15) & ~15
                rows = height
                rowZeroOnly = source && 4 * width < 16
            }
        }

        /// A surface's byte index, or a descriptor's (texel index, byte of it); nil where nothing is held
        func place(_ at: Int, width: Int) -> (index: Int, byte: Int)? {
            if surface { return at < bound ? (base + at, 0) : nil }
            let (y, column) = (at / stored, at % stored)
            guard y < rows, column < rowBytes, !rowZeroOnly || y == 0 else { return nil }
            return (y * width + column / unit, column % unit)
        }
    }

    /// A side's bytes: a surface's, or a descriptor's words and alpha by texel
    struct Bytes {
        var surface: [UInt8] = []
        var words: [UInt32] = [], alpha: [UInt8] = []
        var width = 0
    }

    /// Texel (x, y)'s word and alpha as xr_desc_fill writes them
    static func filled(_ x: Int, _ y: Int, seed: Int) -> (UInt32, UInt8) {
        let r = (37 * x + 101 * y + 13 * seed) & 1023, g = (53 * x + 7 * y + 211 * seed) & 1023
        let b = (11 * x + 89 * y + 97 * seed) & 1023
        return (UInt32(r << 20 | g << 10 | b), UInt8((29 * x + 7 * y + 5 * seed) & 255))
    }

    /// Per destination byte or texel channel, the values the copy may leave there (none: it keeps its value)
    static func model(_ c: Case, _ source: Bytes, _ destinationWidth: Int) -> [[Int: Set<UInt8>]] {
        var writes: [[Int: Set<UInt8>]] = [[:], [:]]
        for (plane, words) in [(0, true), (1, false)] {
            let s = Plane(c.from, words: words, source: true), t = Plane(c.to, words: words, source: false)
            for j in 0 ..< c.size.h {
                for k in 0 ..< c.size.w * s.unit {
                    let from = (c.origin.y + j) * s.pitch + s.unit * c.origin.x + k
                    let to = (c.at.y + j) * t.pitch + t.unit * c.at.x + k
                    guard let into = t.place(to, width: destinationWidth) else { continue }
                    var value: UInt8 = 0
                    if let read = s.place(from, width: source.width) {
                        if s.surface {
                            value = source.surface[read.index]
                        } else {
                            value = words ? UInt8(truncatingIfNeeded: source.words[read.index] >> (8 * read.byte))
                                : source.alpha[read.index]
                        }
                    }
                    writes[plane][t.surface ? into.index : into.index * 4 + into.byte, default: []].insert(value)
                }
            }
        }
        return writes
    }
}

extension XrDescriptorCopies {
    static let surfacePlanes = [
        SurfaceReads.Plane(at: 0, 32, 8, bpe: 4, rows: 128), SurfaceReads.Plane(at: 1024, 32, 8, bpe: 1, rows: 32),
    ]

    /// A made side and its bytes now
    struct Made {
        let texture: MTLTexture
        let surface: IOSurfaceRef?
        var bytes: Bytes
    }

    static func make(_ gpu: GPU, _ side: Side, storage: MTLStorageMode, seed: Int) async throws -> Made {
        switch side {
        case .surface:
            let fill = PlaneCopies.seeded(UInt64(seed))
            guard let s = SurfaceReads.surface("b3a8", 32, 8, planes: surfacePlanes, allocation: 16384, fill: fill),
                  let t = SurfaceReads.texture(gpu, 550, 32, 8, over: s)
            else { throw TestFailure("no 550 over a 'b3a8' 32x8") }
            return Made(texture: t, surface: s, bytes: Bytes(surface: SurfaceReads.bytes(s)))
        case .descriptor(let width, let height):
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: MTLPixelFormat(rawValue: 550)!, width: width, height: height, mipmapped: false
            )
            descriptor.usage = [.shaderRead, .shaderWrite]
            descriptor.storageMode = storage
            guard let t = gpu.device.makeTexture(descriptor: descriptor) else {
                throw TestFailure("no \(width)x\(height) 550 from a descriptor")
            }
            let pipeline = try gpu.compute("xr_desc_fill")
            var value = UInt32(seed)
            let path = try await gpu.run(tolerateError: true) { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(pipeline)
                encoder.setTexture(t, index: 0)
                encoder.setBytes(&value, length: 4, index: 0)
                encoder.dispatchThreads(
                    MTLSize(width: width, height: height, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
                )
                encoder.endEncoding()
            }.facadePath
            guard !path.hasPrefix("error") else { throw TestFailure("the descriptor's fill failed: \(path)") }
            var bytes = Bytes(width: width)
            for y in 0 ..< height {
                for x in 0 ..< width {
                    let (word, alpha) = filled(x, y, seed: seed)
                    bytes.words.append(word)
                    bytes.alpha.append(alpha)
                }
            }
            return Made(texture: t, surface: nil, bytes: bytes)
        }
    }

    /// A descriptor's texels as a kernel reads them: codes rounded back from (code - 384) / 510, alpha bytes
    static func read(_ gpu: GPU, _ texture: MTLTexture) async throws -> (words: [UInt32], alpha: [UInt8]) {
        let texels = try await SurfaceReads.read(gpu, texture)
        let code = { (v: Float) in UInt32(max(0, min(1023, (v * 510 + 384).rounded()))) }
        return (
            texels.map { code($0.x) << 20 | code($0.y) << 10 | code($0.z) },
            texels.map { UInt8(max(0, min(255, ($0.w * 255).rounded()))) }
        )
    }

    public static func run(_ gpu: GPU, _ c: Case, storage: MTLStorageMode) async throws -> Result {
        let from = try await make(gpu, c.from, storage: storage, seed: 3)
        let to = try await make(gpu, c.to, storage: storage, seed: 7)
        let writes = model(c, from.bytes, to.bytes.width)
        let path = try await gpu.run(tolerateError: true) { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: from.texture, sourceSlice: 0, sourceLevel: 0,
                sourceOrigin: MTLOrigin(x: c.origin.x, y: c.origin.y, z: 0),
                sourceSize: MTLSize(width: c.size.w, height: c.size.h, depth: 1), to: to.texture, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin(x: c.at.x, y: c.at.y, z: 0)
            )
            blit.endEncoding()
        }.facadePath
        var r = Result(path: path)
        r.moved = writes[0].count + writes[1].count
        if let surface = to.surface {
            let got = SurfaceReads.bytes(surface)
            r.off = got.indices.filter { i in
                guard let may = writes[0][i] ?? writes[1][i] else { return got[i] != to.bytes.surface[i] }
                return !may.contains(got[i])
            }.count
        } else {
            let got = try await read(gpu, to.texture)
            var off = 0
            for i in got.words.indices {
                for b in 0 ..< 4 {
                    let mask: UInt8 = b == 3 ? 0x3f : 0xff
                    let now = UInt8(truncatingIfNeeded: got.words[i] >> (8 * b)) & mask
                    let was = UInt8(truncatingIfNeeded: to.bytes.words[i] >> (8 * b)) & mask
                    if let may = writes[0][4 * i + b] {
                        off += may.contains { $0 & mask == now } ? 0 : 1
                    } else if now != was {
                        off += 1
                    }
                }
                if let may = writes[1][4 * i] {
                    off += may.contains(got.alpha[i]) ? 0 : 1
                } else if got.alpha[i] != to.bytes.alpha[i] {
                    off += 1
                }
            }
            r.off = off
        }
        if let surface = from.surface {
            let now = SurfaceReads.bytes(surface)
            r.sourceOff = zip(now, from.bytes.surface).filter { $0 != $1 }.count
        } else {
            let now = try await read(gpu, from.texture)
            let words = zip(now.words, from.bytes.words).filter { $0 != $1 & 0x3fff_ffff }.count
            r.sourceOff = words + zip(now.alpha, from.bytes.alpha).filter { $0 != $1 }.count
        }
        return r
    }

    /// A surface's region copied into a descriptor and back out into a second surface at the same coordinates: an M4
    /// Max keeps what its first copy writes into the descriptor's row padding and past its rows, so the second
    /// surface's region holds the first's bytes; bytes of the region off the first surface's
    public static func roundTrip(_ gpu: GPU, width: Int, height: Int, size: (w: Int, h: Int), storage: MTLStorageMode)
        async throws -> Result
    {
        let first = try await make(gpu, .surface, storage: storage, seed: 11)
        let middle = try await make(gpu, d(width, height), storage: storage, seed: 13)
        let last = try await make(gpu, .surface, storage: storage, seed: 17)
        guard let firstSurface = first.surface, let lastSurface = last.surface else { throw TestFailure("no surfaces") }
        var paths: [String] = []
        for (s, t) in [(first.texture, middle.texture), (middle.texture, last.texture)] {
            paths.append(try await gpu.run(tolerateError: true) { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.copy(
                    from: s, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                    sourceSize: MTLSize(width: size.w, height: size.h, depth: 1), to: t, destinationSlice: 0,
                    destinationLevel: 0, destinationOrigin: MTLOrigin()
                )
                blit.endEncoding()
            }.facadePath)
        }
        let a = SurfaceReads.bytes(firstSurface), b = SurfaceReads.bytes(lastSurface)
        let region = (0 ..< size.h).flatMap { j in
            (0 ..< 4 * size.w).map { j * 128 + $0 } + (0 ..< size.w).map { 1024 + j * 128 + $0 }
        }
        let off = region.filter { a[$0] != b[$0] }.count
        return Result(path: paths.first { $0 != "gm20b" } ?? paths[0], off: off, sourceOff: 0, moved: region.count)
    }
}
