import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Copies between textures over two-plane surfaces of one 16 KiB page (gpu.gm20b.planes_plane1_rows,
/// gpu.gm20b.planes_across_formats), each its own command buffer, every byte of each surface seeded, and the
/// destination's whole allocation judged by the rule an M4 Max's copies follow with validation off: plane 0 `w` texels
/// in the destination's unit from each side's plane-0 rows; plane 1 `w` bytes, one a texel and never halved, for `h`
/// rows from plane 1's offset & ~15 at PLANE 0's rows & ~15, on through plane 1's end; 0 where the source byte lies
/// past its last page, nothing where the destination byte does. A plain source over a buffer is read in the
/// destination's unit at its own rows, and plane 1 is left alone.
public enum PlaneCopies {
    public typealias P = SurfaceReads.Plane

    public struct Shape: Sendable {
        public let key: String
        let fourcc: String
        let planes: [P]
    }

    public static let a = Shape(key: "a", fourcc: "b3a8", planes: [
        P(at: 0, 16, 8, bpe: 4, rows: 128), P(at: 1024, 16, 8, bpe: 1, rows: 32),
    ])
    public static let b = Shape(key: "b", fourcc: "b3a8", planes: [
        P(at: 0, 16, 8, bpe: 4, rows: 160), P(at: 2048, 16, 8, bpe: 1, rows: 48),
    ])
    public static let e = Shape(key: "e", fourcc: "b3a8", planes: [
        P(at: 0, 16, 8, bpe: 4, rows: 128), P(at: 16256, 16, 8, bpe: 1, rows: 16),
    ])
    public static let y = Shape(key: "y", fourcc: "420v", planes: [
        P(at: 0, 16, 8, bpe: 1, rows: 48), P(at: 1024, 8, 4, bpe: 2, rows: 32),
    ])
    public static let z = Shape(key: "z", fourcc: "420v", planes: [
        P(at: 0, 16, 8, bpe: 1, rows: 32), P(at: 512, 8, 4, bpe: 2, rows: 48),
    ])

    public static let page = 16384

    public enum Source: Sendable {
        case planes(Shape, format: UInt)
        /// An R8Unorm 16x8 texture over a shared buffer, byte i (13 i + 5) & 255
        case r8OverBuffer
    }

    public struct Case: Sendable {
        public let key: String
        let source: Source
        let from: (x: Int, y: Int)
        let size: (w: Int, h: Int)
        let destination: Shape
        let format: UInt
        let to: (x: Int, y: Int)

        public init(
            _ key: String, _ source: Source, from: (Int, Int), _ size: (Int, Int), into destination: Shape,
            _ format: UInt, at to: (Int, Int)
        ) {
            (self.key, self.source, self.from, self.size) = (key, source, from, size)
            (self.destination, self.format, self.to) = (destination, format, to)
        }
    }

    public struct Result: Sendable {
        public var path = "-"
        /// Destination bytes off the rule
        public var off = 0
        /// Of `off`, those past the destination's plane-1 rows (where only plane 1's run at plane 0's rows reaches)
        public var offPastPlaneOne = 0
        /// Source bytes the copy changed
        public var sourceChanged = 0
        /// Destination bytes the rule changes
        public var moved = 0
        public var made = true
    }

    /// gpu.gm20b.planes_plane1_rows: copies between two textures of one format
    public static let sameFormat: [Case] = [
        Case("a_b_550", .planes(a, format: 550), from: (5, 3), (8, 4), into: b, 550, at: (3, 1)),
        Case("b_a_550", .planes(b, format: 550), from: (0, 0), (16, 8), into: a, 550, at: (0, 0)),
        Case("a_b_551", .planes(a, format: 551), from: (2, 1), (12, 6), into: b, 551, at: (1, 2)),
        Case("a_e_550", .planes(a, format: 550), from: (0, 0), (16, 8), into: e, 550, at: (0, 0)),
        Case("e_a_550", .planes(e, format: 550), from: (3, 0), (10, 8), into: a, 550, at: (2, 0)),
        Case("y_z_500", .planes(y, format: 500), from: (4, 2), (8, 4), into: z, 500, at: (2, 4)),
        Case("z_y_500", .planes(z, format: 500), from: (0, 0), (16, 8), into: y, 500, at: (0, 0)),
    ]

    /// gpu.gm20b.planes_across_formats: copies between textures of two pixel formats, Metal's validation error
    public static let acrossFormats: [Case] = [
        Case("r8_a_550", .r8OverBuffer, from: (2, 1), (4, 4), into: a, 550, at: (3, 2)),
        Case("a550_b551", .planes(a, format: 550), from: (2, 2), (4, 4), into: b, 551, at: (4, 2)),
        Case("b551_a550", .planes(b, format: 551), from: (2, 2), (4, 4), into: a, 550, at: (4, 2)),
        Case("y500_b550", .planes(y, format: 500), from: (2, 2), (4, 4), into: b, 550, at: (4, 2)),
    ]

    static func unit(_ format: UInt) -> Int { format == 550 || format == 551 ? 4 : 1 }

    /// splitmix64 of `seed` + i, its low byte: every surface's bytes, unique per seed
    static func seeded(_ seed: UInt64) -> (Int) -> UInt8 {
        { i in
            var z = seed &+ UInt64(i) &* 0x9e37_79b9_7f4a_7c15
            z = (z ^ (z >> 30)) &* 0xbf58_476d_1ce4_e5b9
            z = (z ^ (z >> 27)) &* 0x94d0_49bb_1331_11eb
            return UInt8(truncatingIfNeeded: z ^ (z >> 31))
        }
    }

    static func surface(_ s: Shape, seed: UInt64) -> IOSurfaceRef? {
        SurfaceReads.surface(s.fourcc, 16, 8, planes: s.planes, allocation: page, fill: seeded(seed))
    }

    public static func run(_ gpu: GPU, _ c: Case) async throws -> Result {
        guard let into = surface(c.destination, seed: 0x5eed_0000 + UInt64(c.key.utf8.count)) else {
            throw TestFailure("\(c.key): no \(c.destination.fourcc) surface")
        }
        guard let destination = SurfaceReads.texture(gpu, c.format, 16, 8, over: into) else {
            return Result(made: false)
        }
        let before = SurfaceReads.bytes(into)
        let u = unit(c.format)
        var want = before
        let sourceBytes: [UInt8], sourceTexture: MTLTexture, sourceAfter: () -> [UInt8]
        let sourceRows: Int, sourceZero: Int, sourcePlaneOne: (offset: Int, rows: Int)?
        let keep: AnyObject
        switch c.source {
        case .planes(let shape, let format):
            guard let from = surface(shape, seed: 0x00c0_ffee) else { throw TestFailure("\(c.key): no source surface") }
            guard let texture = SurfaceReads.texture(gpu, format, 16, 8, over: from) else { return Result(made: false) }
            (sourceTexture, keep) = (texture, from)
            sourceBytes = SurfaceReads.bytes(from)
            sourceAfter = { SurfaceReads.bytes(from) }
            sourceRows = shape.planes[0].pitch & ~15
            sourcePlaneOne = (shape.planes[1].offset & ~15, sourceRows)
            sourceZero = shape.planes[0].offset & ~15
        case .r8OverBuffer:
            let pitch = max(16, gpu.device.minimumLinearTextureAlignment(for: .r8Unorm))
            let buffer = try gpu.buffer((0 ..< pitch * 8).map { UInt8(truncatingIfNeeded: 13 * $0 + 5) })
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .r8Unorm, width: 16, height: 8, mipmapped: false
            )
            descriptor.storageMode = .shared
            descriptor.usage = [.shaderRead]
            guard let texture = buffer.makeTexture(descriptor: descriptor, offset: 0, bytesPerRow: pitch) else {
                throw TestFailure("\(c.key): no R8Unorm texture over a buffer of rows \(pitch)")
            }
            (sourceTexture, keep) = (texture, buffer)
            sourceBytes = [UInt8](UnsafeRawBufferPointer(start: buffer.contents(), count: buffer.length))
            sourceAfter = { [UInt8](UnsafeRawBufferPointer(start: buffer.contents(), count: buffer.length)) }
            (sourceRows, sourcePlaneOne, sourceZero) = (pitch, nil, 0)
        }
        let rows = c.destination.planes[0].pitch & ~15, planeZero = c.destination.planes[0].offset & ~15
        let read = { (i: Int) in i < sourceBytes.count ? sourceBytes[i] : 0 }
        for k in 0 ..< c.size.h {
            for i in 0 ..< c.size.w * u {
                let at = planeZero + (c.to.y + k) * rows + c.to.x * u + i
                if at < want.count { want[at] = read(sourceZero + (c.from.y + k) * sourceRows + c.from.x * u + i) }
            }
        }
        let planeOne = c.destination.planes[1].offset & ~15
        if let (offset, sourceStep) = sourcePlaneOne {
            for k in 0 ..< c.size.h {
                for i in 0 ..< c.size.w {
                    let at = planeOne + (c.to.y + k) * rows + c.to.x + i
                    if at < want.count { want[at] = read(offset + (c.from.y + k) * sourceStep + c.from.x + i) }
                }
            }
        }
        let path = try await gpu.run(tolerateError: true) { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: sourceTexture, sourceSlice: 0, sourceLevel: 0,
                sourceOrigin: MTLOrigin(x: c.from.x, y: c.from.y, z: 0),
                sourceSize: MTLSize(width: c.size.w, height: c.size.h, depth: 1), to: destination, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin(x: c.to.x, y: c.to.y, z: 0)
            )
            blit.endEncoding()
        }.facadePath
        let got = SurfaceReads.bytes(into)
        let planeOneEnd = c.destination.planes[1].offset + c.destination.planes[1].pitch * c.destination.planes[1]
            .height
        let wrong = got.indices.filter { got[$0] != want[$0] }
        let after = sourceAfter()
        withExtendedLifetime(keep) {}
        return Result(
            path: path, off: wrong.count, offPastPlaneOne: wrong.filter { $0 >= planeOneEnd }.count,
            sourceChanged: zip(after, sourceBytes).filter { $0 != $1 }.count,
            moved: zip(want, before).filter { $0 != $1 }.count
        )
    }
}
