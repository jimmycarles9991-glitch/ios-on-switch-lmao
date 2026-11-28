import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Plain-format textures over a surface plane whose rows, as an M4 Max's GPU reads them (bytesPerRow rounded down to
/// 16), overlap, lie under 16 bytes or run past the allocation (gpu.gm20b.plain_rows_*). An M4 Max makes each at any
/// extent and reads plane row y at y * (bytesPerRow & ~15), rows under 16 bytes a mebibyte apart and 0 past the
/// allocation's last page; its GPU stores rows under 16 bytes 16 apart, its CPU copies at the plane's own bytesPerRow,
/// the last row winning. Every texel's code tells which plane row a byte several rows reach kept
/// (shaders/PlainRows.metal).
public enum PlainRows {
    public struct Reading: Sendable {
        public var made = false
        public var path = "-"
        /// Texels or bytes off Apple's, of `judged`
        public var off = -1
        public var judged = 0
        /// The first one off: "<texel or byte> got <value> want <value>"
        public var first = "-"
    }

    /// A plane of `width` x `height` texels of `bpp` bytes, `pitch` bytes a row, under a texture of `format` turned by
    /// `turn` (its descriptor the frame: the plane's extent, quarter turns swapped)
    public struct Shape: Sendable {
        public let format: UInt
        public let width, height, bpp, pitch: Int
        public var turn = 0

        public init(_ format: UInt, _ width: Int, _ height: Int, bpp: Int, pitch: Int, turn: Int = 0) {
            (self.format, self.width, self.height) = (format, width, height)
            (self.bpp, self.pitch, self.turn) = (bpp, pitch, turn)
        }

        /// The rows' stride as the GPU stores them (and reads them, but for rows under 16 bytes: a mebibyte apart)
        public var stride: Int { pitch < 16 ? 16 : pitch & ~15 }
        /// The bytes from the plane's start its stored rows reach
        public var span: Int { stride * (height - 1) + width * bpp }
        public var frame: (width: Int, height: Int) { turn & 1 == 1 ? (height, width) : (width, height) }

        /// The rows whose texels reach plane byte `a`, in order
        public func rows(reaching a: Int) -> [Int] {
            (0 ..< height).filter { a - stride * $0 >= 0 && a - stride * $0 < width * bpp }
        }
    }

    public static let page = Int(getpagesize())

    /// Byte i of a surface a read judges: every byte differs from its neighbours
    public static func seeded(_ i: Int) -> UInt8 { UInt8((UInt32(truncatingIfNeeded: i) &* 2_654_435_761) >> 24) }

    /// Plane texel (x, y)'s bytes as shaders/PlainRows.metal stores them: one byte (x + 7y) & 255; of more, byte b the
    /// low or high byte of (7x + 131y + 1) & 0xffff by b's parity, XOR 0x11 for each pair before it
    public static func code(_ bpp: Int, _ x: Int, _ y: Int) -> [UInt8] {
        if bpp == 1 { return [UInt8((x + 7 * y) & 255)] }
        let c = (x * 7 + y * 131 + 1) & 0xffff
        return (0 ..< bpp).map { UInt8(truncatingIfNeeded: ($0 & 1 == 1 ? c >> 8 : c) ^ ($0 / 2) * 0x11) }
    }

    /// A '420v' whose plane 0 holds `s`'s rows, declared as wide as its bytesPerRow holds (IOSurfaceCreate makes no
    /// plane wider), a one-byte plane 1 on the next 64 bytes; the allocation the span's 16 KiB pages and one more
    /// unless given. Every mapped byte `fill(i)`, the page's bytes past the allocation too.
    static func surface(_ s: Shape, allocation: Int? = nil, fill: (Int) -> UInt8) -> IOSurfaceRef? {
        let plane0 = s.pitch * s.height
        let pages = (max(plane0, s.span) + 16383) / 16384 * 16384 + 16384
        let planes = [
            TurnedOverlapTiles.Plane(
                offset: 0, width: max(1, min(s.width, s.pitch / s.bpp)), height: s.height, bpe: s.bpp, pitch: s.pitch
            ),
            TurnedOverlapTiles.Plane(offset: (plane0 + 63) & ~63, width: 1, height: 1, bpe: 1, pitch: 16),
        ]
        let declared = planes[0].width
        guard
            let surface = TurnedOverlapTiles.surface(
                "420v", declared, s.height, planes, allocation: allocation ?? pages
            )
        else { return nil }
        seed(surface, fill)
        return surface
    }

    static func seed(_ surface: IOSurfaceRef, _ fill: (Int) -> UInt8) {
        let mapped = (IOSurfaceGetAllocSize(surface) + page - 1) / page * page
        RowsUnder16.edit(surface) { base in for i in 0 ..< mapped { base[i] = fill(i) } }
    }

    static func mappedBytes(_ surface: IOSurfaceRef) -> [UInt8] {
        let mapped = (IOSurfaceGetAllocSize(surface) + page - 1) / page * page
        IOSurfaceLock(surface, .readOnly, nil)
        defer { IOSurfaceUnlock(surface, .readOnly, nil) }
        return [UInt8](UnsafeRawBufferPointer(start: IOSurfaceGetBaseAddress(surface), count: mapped))
    }

    static func texture(
        _ gpu: GPU, _ s: Shape, over surface: IOSurfaceRef,
        usage: MTLTextureUsage = [.shaderRead, .shaderWrite, .renderTarget]
    ) -> MTLTexture? {
        let frame = s.frame
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: s.format)!, width: frame.width, height: frame.height,
            mipmapped: false
        )
        descriptor.usage = usage
        descriptor.storageMode = .shared
        if s.turn != 0 {
            guard descriptor.responds(to: NSSelectorFromString("setRotation:")) else { return nil }
            descriptor.usage = [.renderTarget]
            descriptor.setValue(NSNumber(value: s.turn), forKey: "rotation")
            descriptor.setValue(NSNumber(value: true), forKey: "framebufferOnly")
        }
        return gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0)
    }

    /// Every texel of `texture` as read_float_texture reads it, row by row, and the read's facade path
    static func read(_ gpu: GPU, _ texture: MTLTexture) async throws -> (path: String, texels: [SIMD4<Float>]) {
        let count = texture.width * texture.height
        let out = try gpu.buffer(count * 16)
        let pipeline = try gpu.compute("read_float_texture")
        let path = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: texture.width, height: texture.height, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
            )
            encoder.endEncoding()
        }.facadePath
        let start = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: count)
        let texels = UnsafeBufferPointer(start: start, count: count)
        return (path, Array(texels))
    }

    /// `got` against `want`, texel by texel within half a code
    static func judged(_ r: inout Reading, _ got: [SIMD4<Float>], _ want: (Int) -> SIMD4<Float>) {
        r.judged = got.count
        let half = SIMD4<Float>(repeating: 0.5 / 255)
        let off = got.indices.filter { i in
            let d = got[i] - want(i)
            return any(d .> half) || any(d .< -half)
        }
        r.off = off.count
        if let i = off.first { r.first = "\(i)_got_\(got[i] * 255)_want_\(want(i) * 255)" }
    }

    /// R8Unorm 136x104 over a '420v' 136x104 whose plane 0 is declared 16 wide with rows of 16 bytes, plane 1 at 16384
    /// in 32 KiB, every byte seeded: texel (x, y) reads plane byte y * 16 + x
    public static func overlapReads(_ gpu: GPU) async throws -> Reading {
        let planes = [
            TurnedOverlapTiles.Plane(offset: 0, width: 16, height: 104, bpe: 1, pitch: 16),
            TurnedOverlapTiles.Plane(offset: 16384, width: 8, height: 52, bpe: 2, pitch: 16),
        ]
        guard let surface = TurnedOverlapTiles.surface("420v", 136, 104, planes, allocation: 32768) else {
            throw TestFailure("no '420v' of rows 16")
        }
        seed(surface, seeded)
        var r = Reading()
        let shape = Shape(MTLPixelFormat.r8Unorm.rawValue, 136, 104, bpp: 1, pitch: 16)
        guard let texture = texture(gpu, shape, over: surface) else { return r }
        r.made = true
        let (path, got) = try await read(gpu, texture)
        r.path = path
        judged(&r, got) { SIMD4(Float(seeded($0 / 136 * 16 + $0 % 136)) / 255, 0, 0, 1) }
        return r
    }

    /// BGRA8Unorm 64x1048 over the one plane of a 'BGRA' 64x48 of rows 256 in 12 KiB, its page seeded: rows inside the
    /// allocation's 16 KiB page read its bytes (rows 48-63 the page's bytes past the allocation), rows past it 0
    public static func pastPageReads(_ gpu: GPU) async throws -> Reading {
        let plane = [TurnedOverlapTiles.Plane(offset: 0, width: 64, height: 48, bpe: 4, pitch: 256)]
        guard let surface = TurnedOverlapTiles.surface("BGRA", 64, 48, plane, allocation: 12288) else {
            throw TestFailure("no 'BGRA' 64x48 of one plane in 12 KiB")
        }
        seed(surface, seeded)
        let mapped = (IOSurfaceGetAllocSize(surface) + page - 1) / page * page
        var r = Reading()
        let shape = Shape(MTLPixelFormat.bgra8Unorm.rawValue, 64, 1048, bpp: 4, pitch: 256)
        guard let texture = texture(gpu, shape, over: surface, usage: [.shaderRead]) else { return r }
        r.made = true
        let (path, got) = try await read(gpu, texture)
        r.path = path
        judged(&r, got) { i in
            let a = i / 64 * 256 + i % 64 * 4
            guard a + 4 <= mapped else { return .zero }
            return SIMD4(Float(seeded(a + 2)), Float(seeded(a + 1)), Float(seeded(a)), Float(seeded(a + 3))) / 255
        }
        return r
    }

    /// R8Unorm 136x40 over rows of 64 bytes: replaceRegion of the whole texture with every texel's code at bytesPerRow
    /// 136 (writeOff: plane bytes not the code of the last row reaching them, or changed outside the span), then
    /// getBytes of the whole texture (readOff: texel (x, y) not plane byte y * 64 + x)
    public static func cpuCopies(_ gpu: GPU) throws -> (made: Bool, writeOff: Int, readOff: Int, first: String) {
        let s = Shape(MTLPixelFormat.r8Unorm.rawValue, 136, 40, bpp: 1, pitch: 64)
        guard let surface = surface(s, fill: { _ in 0xee }) else { throw TestFailure("no '420v' of rows 64") }
        guard let texture = texture(gpu, s, over: surface) else { return (false, -1, -1, "-") }
        let codes = (0 ..< s.width * s.height).map { code(1, $0 % s.width, $0 / s.width)[0] }
        texture.replace(
            region: MTLRegionMake2D(0, 0, s.width, s.height), mipmapLevel: 0, withBytes: codes, bytesPerRow: s.width
        )
        let bytes = mappedBytes(surface)
        var first = "-"
        let writeOff = (0 ..< bytes.count).filter { a in
            let want = a < s.span ? s.rows(reaching: a).last.map { code(1, a - 64 * $0, $0)[0] } ?? 0xee : 0xee
            if bytes[a] != want && first == "-" { first = "byte_\(a)_got_\(bytes[a])_want_\(want)" }
            return bytes[a] != want
        }.count
        var read = [UInt8](repeating: 0, count: codes.count)
        texture.getBytes(&read, bytesPerRow: s.width, from: MTLRegionMake2D(0, 0, s.width, s.height), mipmapLevel: 0)
        let readOff = read.indices.filter { read[$0] != bytes[$0 / s.width * 64 + $0 % s.width] }.count
        return (true, writeOff, readOff, first)
    }
}
