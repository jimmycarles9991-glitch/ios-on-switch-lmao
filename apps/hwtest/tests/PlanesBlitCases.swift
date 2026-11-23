import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// The blits gpu.gm20b.planes_blits runs (PlanesBlitsTest.swift), each over memory seeded at random as blitgroups.mm seeds
/// it, and judged whole: every byte of each surface (to the end of its last page), buffer and plain texture afterwards
/// against what it held before with the rule's bytes written in. p0(x, y) and p1(x, y) are byte x of row y of a plane
/// from the plane's start at the surface's own pitch, x unbounded: past a row's end they are the next rows' bytes, past
/// plane 0 the surface's bytes after it.
enum PlanesBlitCases {
    struct Planes {
        let key: String
        let fourcc: OSType
        let format: UInt
        /// The bytes of plane 0 a texel of the texture counts in a blit (u)
        let unit: Int
        /// A one-plane surface's IOSurface element width in pixels; 0 for two planes
        let elementWidth: Int
        /// Made with CVPixelBufferCreate unless the case asks for an exact pitch
        let pixelBuffer: Bool

        var twoPlanes: Bool { elementWidth == 0 }

        init(
            _ key: String, _ fourcc: OSType, _ format: UInt, unit: Int, elementWidth: Int = 0, pixelBuffer: Bool = true
        ) {
            (self.key, self.fourcc, self.format, self.unit) = (key, fourcc, format, unit)
            (self.elementWidth, self.pixelBuffer) = (elementWidth, pixelBuffer)
        }
    }

    struct Plain {
        let key: String
        let format: MTLPixelFormat
        let bytes: Int
    }

    struct Result {
        var path = "-"
        /// Bytes off the rule's, by the memory they lie in
        var off: [(String, Int)] = []
        /// Bytes of a region the rule leaves unjudged that changed
        var unjudged = 0

        var changed: Int { off.reduce(0) { $0 + $1.1 } }
        var text: String { off.map { "\($0.0) \($0.1)" }.joined(separator: ", ") }
    }

    /// `skips`: the calls the driver counts skipped (skipped.call.blit): a copy between two pixel formats, a fill of
    /// planes, a region past its surface's last page
    typealias Case = (key: String, skips: Int, run: (GPU) async throws -> Result)

    static let yuv420 = Planes("420v", kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, 500, unit: 1)
    static let x420 = Planes("x420", YcbcrPackedTargetTest.x420, 505, unit: 2)
    static let p420 = Planes("p420", YcbcrPackedTargetTest.p420, 508, unit: 1)
    static let vuy = Planes("2vuy", kCVPixelFormatType_422YpCbCr8, 501, unit: 2, elementWidth: 2)
    static let xr = Planes("b3a8", kCVPixelFormatType_30RGBLE_8A_BiPlanar, 550, unit: 4, pixelBuffer: false)
    static let v504 = Planes("504", kCVPixelFormatType_444YpCbCr10, 504, unit: 4, elementWidth: 1, pixelBuffer: false)
    static let v524 = Planes("524", kCVPixelFormatType_444YpCbCr10, 524, unit: 4, elementWidth: 1, pixelBuffer: false)
    static let v562 = Planes("562", kCVPixelFormatType_422YpCbCr8, 562, unit: 2, elementWidth: 2, pixelBuffer: false)
    static let planes = [yuv420, x420, p420, vuy, xr, v504, v524, v562]
    static let exactPitch = [yuv420, x420, vuy, xr, v504, v562]

    static let plains = [
        Plain(key: "r8", format: .r8Unorm, bytes: 1), Plain(key: "rg8", format: .rg8Unorm, bytes: 2),
        Plain(key: "r16", format: .r16Unorm, bytes: 2), Plain(key: "rgba8", format: .rgba8Unorm, bytes: 4),
    ]
    static let rgba16u = Plain(key: "rgba16u", format: .rgba16Uint, bytes: 8)
    static let rgba32u = Plain(key: "rgba32u", format: .rgba32Uint, bytes: 16)
    static let xrPlains = plains + [Plain(key: "r32u", format: .r32Uint, bytes: 4), rgba16u]

    static let pairs: [(Planes, Planes)] = [
        (yuv420, yuv420), (x420, x420), (p420, p420), (yuv420, x420), (xr, xr), (xr, yuv420), (vuy, vuy), (yuv420, vuy),
        (v504, v504), (v562, v562), (v562, vuy), (vuy, v562), (xr, v504),
    ]

    static var all: [Case] {
        var cases: [Case] = []
        for p in planes { cases.append(("a_\(p.key)", 0, { try await fromBuffer($0, p) })) }
        for p in planes { cases.append(("b_\(p.key)", 0, { try await toBuffer($0, p) })) }
        for p in planes {
            for q in p.key == xr.key ? xrPlains : plains {
                cases.append(("c_\(p.key)_\(q.key)", 1, { try await outOf($0, Surface($0, p, 16, 8), q) }))
            }
        }
        for p in exactPitch {
            for q in [rgba16u, rgba32u] {
                cases.append(
                    ("c_\(p.key)_exact_\(q.key)", 1, { try await outOf($0, Surface($0, p, 16, 8, exact: true), q) })
                )
            }
        }
        for p in planes {
            for q in p.key == xr.key ? xrPlains : plains {
                cases.append(("d_\(p.key)_\(q.key)", 1, { try await into($0, p, q) }))
            }
        }
        for (s, d) in pairs {
            cases.append(("e_\(s.key)_\(d.key)", s.format == d.format ? 0 : 1, { try await between($0, s, d) }))
        }
        for p in planes { cases.append(("f_\(p.key)", 1, { try await fill($0, p) })) }
        cases.append(("g_past_page", 1, { try await outOf($0, lastPage($0), rgba32u, from: (8, 508), lands: false) }))
        cases.append(("g_in_page", 1, { try await outOf($0, lastPage($0), rgba32u, from: (2, 500)) }))
        return cases
    }

    static let line = 128

    /// A: buffer rows into (6,2) 12x3 of a 24x8 surface: p0(6u + i, 2 + k) = buf(i, k) for i < 12u
    static func fromBuffer(_ gpu: GPU, _ p: Planes) async throws -> Result {
        let s = try Surface(gpu, p, 24, 8)
        let before = s.seeded(104_729)
        let data = random(31_337, line * 3)
        let buffer = try gpu.buffer(data)
        let path = try await blit(gpu) {
            $0.copy(
                from: buffer, sourceOffset: 0, sourceBytesPerRow: line, sourceBytesPerImage: line * 3,
                sourceSize: MTLSize(width: 12, height: 3, depth: 1), to: s.texture, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin(x: 6, y: 2, z: 0)
            )
        }
        let u = p.unit
        let writes = rect(12 * u, 3) { i, k in (s.p0(6 * u + i, 2 + k), data[k * line + i]) }
        return Result(path: path, off: s.off(before, writes) + [("buffer", wrong(contents(buffer), data))])
    }

    /// B: the same region into buffer rows: buf(i, k) = p0(6u + i, 2 + k) for i < 12u, nothing past 12u in a row
    static func toBuffer(_ gpu: GPU, _ p: Planes) async throws -> Result {
        let s = try Surface(gpu, p, 24, 8)
        let before = s.seeded(7_919)
        let seeded = random(31_337, line * 3 + 64)
        let buffer = try gpu.buffer(seeded)
        let path = try await blit(gpu) {
            $0.copy(
                from: s.texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 6, y: 2, z: 0),
                sourceSize: MTLSize(width: 12, height: 3, depth: 1), to: buffer, destinationOffset: 0,
                destinationBytesPerRow: line, destinationBytesPerImage: line * 3
            )
        }
        let u = p.unit
        let expected = patched(seeded, rect(12 * u, 3) { i, k in (k * line + i, before[s.p0(6 * u + i, 2 + k)]) })
        return Result(path: path, off: s.off(before, []) + [("buffer", wrong(contents(buffer), expected))])
    }

    /// C and G: 8x4 texels from `from` into a 16x8 plain texture at (1,1), plane 0 read as one run in its texel of B
    /// bytes: plain(B + i, 1 + k) = p0(from.x * B + i, from.y + k) for i < 8B; nothing unless `lands`
    static func outOf(
        _ gpu: GPU, _ s: Surface, _ q: Plain, from: (x: Int, y: Int) = (2, 2), lands: Bool = true
    ) async throws -> Result {
        let before = s.seeded(6_007)
        let (plain, seeded) = try await plainTexture(gpu, q)
        let path = try await copied(gpu, s.texture, plain, from: from)
        let b = q.bytes, row = 16 * q.bytes
        let writes =
            lands ? rect(8 * b, 4) { i, k in ((1 + k) * row + b + i, before[s.p0(from.x * b + i, from.y + k)]) } : []
        let got = try await gpu.read(plain, bytesPerPixel: b)
        return Result(path: path, off: s.off(before, []) + [("plain", wrong(got, patched(seeded, writes)))])
    }

    /// G's surface: '2vuy' 16x512 of exactly one 16 KiB page, rows 32 bytes apart
    static func lastPage(_ gpu: GPU) throws -> Surface { try Surface(gpu, vuy, 16, 512, exact: true) }

    /// D: a plain texture's 8x4 texels from (2,2) into plane 0 at (1,1): where its texel is plane 0's unit,
    /// p0(u + i, 1 + k) = plain(2u + i, 2 + k) for i < 8u, plane 1 as it was; of another texel size its bytes are read
    /// in plane 0's unit in its own memory layout, which on Apple's GPU is not GM20B's, so those 8u x 4 bytes are
    /// reported, not judged, and every other byte must hold
    static func into(_ gpu: GPU, _ p: Planes, _ q: Plain) async throws -> Result {
        let s = try Surface(gpu, p, 16, 8)
        let before = s.seeded(104_729)
        let (plain, seeded) = try await plainTexture(gpu, q)
        let path = try await copied(gpu, plain, s.texture)
        let u = p.unit, row = 16 * q.bytes
        let region = rect(8 * u, 4) { i, k in (s.p0(u + i, 1 + k), seeded[(2 + k) * row + 2 * u + i]) }
        let got = try await gpu.read(plain, bytesPerPixel: q.bytes)
        guard q.bytes != u else {
            return Result(path: path, off: s.off(before, region) + [("plain", wrong(got, seeded))])
        }
        let unjudged = Set(region.map(\.0)), after = s.bytes
        return Result(
            path: path, off: s.off(before, [], ignoring: unjudged) + [("plain", wrong(got, seeded))],
            unjudged: unjudged.filter { after[$0] != before[$0] }.count
        )
    }

    /// E: 8x4 texels from (2,2) to (1,1) between two planes' textures, plane 0 in the destination's unit:
    /// p0d(ud + i, 1 + k) = p0s(2ud + i, 2 + k) for i < 8ud; between two with a plane 1, 8 bytes of it, one a texel,
    /// for 4 rows from each side's plane-1 offset & ~15 at its plane-0 rows & ~15, on through plane 1's end, 0 read past
    /// the source's last page, none written past the destination's
    static func between(_ gpu: GPU, _ a: Planes, _ b: Planes) async throws -> Result {
        let src = try Surface(gpu, a, 16, 8), dst = try Surface(gpu, b, 16, 8)
        let from = src.seeded(3_571), before = dst.seeded(104_729)
        let path = try await copied(gpu, src.texture, dst.texture)
        let u = b.unit
        var writes = rect(8 * u, 4) { i, k in (dst.p0(u + i, 1 + k), from[src.p0(2 * u + i, 2 + k)]) }
        if a.twoPlanes && b.twoPlanes {
            let read = { (j: Int, r: Int) -> UInt8 in
                let at = (src.offset[1] & ~15) + (2 + r) * (src.pitch[0] & ~15) + 2 + j
                return at < src.mapped ? from[at] : 0
            }
            let at = { (j: Int, r: Int) in (dst.offset[1] & ~15) + (1 + r) * (dst.pitch[0] & ~15) + 1 + j }
            writes += rect(8, 4) { j, r in (at(j, r), read(j, r)) }.filter { $0.0 < dst.mapped }
        }
        return Result(path: path, off: dst.off(before, writes) + [("source", wrong(src.bytes, from))])
    }

    /// F: a colour fill of (5,1) 6x3: nothing written
    static func fill(_ gpu: GPU, _ p: Planes) async throws -> Result {
        let s = try Surface(gpu, p, 16, 8)
        let before = s.seeded(104_729)
        guard
            let path = try await FillShapeCases.filled(
                gpu, s.texture, region: MTLRegionMake2D(5, 1, 6, 3),
                MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1)
            )
        else { throw TestFailure("the blit encoder answers no fillTexture:level:slice:region:color:") }
        return Result(path: path, off: s.off(before, []))
    }
}

extension PlanesBlitCases {
    /// A planes' texture over a surface made as the probes make it (blitgroups.mm surface(), bound.mm and units.mm
    /// make()): 'b3a8' with IOSurfaceCreate, its planes' rows 64-byte aligned; one-plane kinds with IOSurfaceCreate at
    /// rows of exactly w * u bytes; two-plane YCbCr through CVPixelBufferCreate, or at an exact pitch with IOSurfaceCreate
    struct Surface {
        let surface: IOSurfaceRef
        let owner: CVPixelBuffer?
        let texture: MTLTexture
        let offset: [Int]
        let pitch: [Int]
        let allocated: Int
        /// To the end of the allocation's last page, which Metal's one-run reads reach
        let mapped: Int

        init(_ gpu: GPU, _ planes: Planes, _ w: Int, _ h: Int, exact: Bool = false) throws {
            var owner: CVPixelBuffer?
            let made: IOSurfaceRef?
            let u = planes.unit
            if planes.key == xr.key {
                made = Self.twoPlanes(planes, w, h, pitch: (w * 4 + 63) & ~63, chroma: (w, h, (w + 63) & ~63, 1))
            } else if planes.elementWidth > 0 && (exact || !planes.pixelBuffer) {
                made = IOSurfaceCreate(
                    [
                        kIOSurfaceWidth: w, kIOSurfaceHeight: h, kIOSurfacePixelFormat: planes.fourcc,
                        kIOSurfaceBytesPerRow: w * u, kIOSurfaceBytesPerElement: 4,
                        kIOSurfaceElementWidth: planes.elementWidth, kIOSurfaceAllocSize: w * u * h,
                    ] as [CFString: Any] as CFDictionary
                )
            } else if exact {
                made = Self.twoPlanes(planes, w, h, pitch: w * u, chroma: (w / 2, h / 2, w * u, 2 * u))
            } else {
                let attributes =
                    [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true]
                        as CFDictionary
                guard CVPixelBufferCreate(nil, w, h, planes.fourcc, attributes, &owner) == kCVReturnSuccess,
                      let buffer = owner
                else { throw TestFailure("no \(planes.key) pixel buffer") }
                made = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue()
            }
            guard let s = made else { throw TestFailure("no \(planes.key) surface") }
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: MTLPixelFormat(rawValue: planes.format)!, width: w, height: h, mipmapped: false
            )
            descriptor.usage = [.shaderRead, .shaderWrite]
            guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: s, plane: 0) else {
                throw TestFailure("no \(planes.format) texture over \(planes.key)")
            }
            (surface, self.owner, self.texture) = (s, owner, texture)
            let count = IOSurfaceGetPlaneCount(s), base = Int(bitPattern: IOSurfaceGetBaseAddress(s))
            offset =
                count == 0 ? [0] : (0 ..< count).map { Int(bitPattern: IOSurfaceGetBaseAddressOfPlane(s, $0)) - base }
            pitch =
                count == 0 ? [IOSurfaceGetBytesPerRow(s)] : (0 ..< count).map { IOSurfaceGetBytesPerRowOfPlane(s, $0) }
            let page = Int(vm_page_size)
            allocated = IOSurfaceGetAllocSize(s)
            mapped = (allocated + page - 1) / page * page
        }

        /// Plane 0 of `pitch`-byte rows of u-byte pixels, then plane 1 (`chroma`: its width, height, pitch and bytes a
        /// pixel) right after it
        static func twoPlanes(
            _ p: Planes, _ w: Int, _ h: Int, pitch: Int, chroma: (w: Int, h: Int, pitch: Int, bytes: Int)
        ) -> IOSurfaceRef? {
            let properties: [CFString: Any] = [
                kIOSurfaceWidth: w, kIOSurfaceHeight: h, kIOSurfacePixelFormat: p.fourcc,
                kIOSurfaceAllocSize: pitch * h + chroma.pitch * chroma.h,
                kIOSurfacePlaneInfo: [
                    [
                        kIOSurfacePlaneWidth: w, kIOSurfacePlaneHeight: h, kIOSurfacePlaneBytesPerRow: pitch,
                        kIOSurfacePlaneBytesPerElement: p.unit, kIOSurfacePlaneOffset: 0,
                        kIOSurfacePlaneSize: pitch * h,
                    ],
                    [
                        kIOSurfacePlaneWidth: chroma.w, kIOSurfacePlaneHeight: chroma.h,
                        kIOSurfacePlaneBytesPerRow: chroma.pitch, kIOSurfacePlaneBytesPerElement: chroma.bytes,
                        kIOSurfacePlaneOffset: pitch * h, kIOSurfacePlaneSize: chroma.pitch * chroma.h,
                    ],
                ],
            ]
            return IOSurfaceCreate(properties as CFDictionary)
        }

        func p0(_ x: Int, _ y: Int) -> Int { offset[0] + y * pitch[0] + x }
        func p1(_ x: Int, _ y: Int) -> Int { offset[1] + y * pitch[1] + x }

        var bytes: [UInt8] {
            IOSurfaceLock(surface, .readOnly, nil)
            defer { IOSurfaceUnlock(surface, .readOnly, nil) }
            return Array(UnsafeRawBufferPointer(start: IOSurfaceGetBaseAddress(surface), count: mapped))
        }

        /// The allocation's bytes from std::mt19937(seed), as blitgroups.mm's fill(); the bytes to the end of its last
        /// page then
        func seeded(_ seed: UInt32) -> [UInt8] {
            let values = random(seed, allocated)
            IOSurfaceLock(surface, [], nil)
            values.withUnsafeBytes {
                IOSurfaceGetBaseAddress(surface).copyMemory(from: $0.baseAddress!, byteCount: $0.count)
            }
            IOSurfaceUnlock(surface, [], nil)
            return bytes
        }

        /// Bytes off `before` with `writes` in, but those `ignoring`: plane 0's (with whatever precedes plane 1), plane
        /// 1's, and the last page's past the allocation
        func off(_ before: [UInt8], _ writes: [(Int, UInt8)], ignoring: Set<Int> = []) -> [(String, Int)] {
            let split = offset.count > 1 ? offset[1] : allocated
            let at = wrongAt(bytes, patched(before, writes)).filter { !ignoring.contains($0) }
            return [
                ("p0", at.filter { $0 < split }.count), ("p1", at.filter { $0 >= split && $0 < allocated }.count),
                ("tail", at.filter { $0 >= allocated }.count),
            ]
        }
    }

    static func random(_ seed: UInt32, _ count: Int) -> [UInt8] {
        P3YcbcrStoreTest.mt19937(seed: seed, count: count).map { UInt8(truncatingIfNeeded: $0) }
    }

    /// The `width` x `rows` bytes of a rule, each as (where it lands, the byte it holds)
    static func rect(_ width: Int, _ rows: Int, _ byte: (Int, Int) -> (Int, UInt8)) -> [(Int, UInt8)] {
        (0 ..< rows).flatMap { k in (0 ..< width).map { i in byte(i, k) } }
    }

    static func patched(_ bytes: [UInt8], _ writes: [(Int, UInt8)]) -> [UInt8] {
        var out = bytes
        for (at, byte) in writes { out[at] = byte }
        return out
    }

    static func wrongAt(_ got: [UInt8], _ expected: [UInt8]) -> [Int] {
        zip(got, expected).enumerated().filter { $0.element.0 != $0.element.1 }.map(\.offset)
    }

    static func wrong(_ got: [UInt8], _ expected: [UInt8]) -> Int { wrongAt(got, expected).count }

    static func contents(_ buffer: MTLBuffer) -> [UInt8] {
        [UInt8](UnsafeRawBufferPointer(start: buffer.contents(), count: buffer.length))
    }

    /// A 16x8 shared texture of `q`, every byte from std::mt19937(4099) as blitgroups.mm seeds a plain source; those bytes
    static func plainTexture(_ gpu: GPU, _ q: Plain) async throws -> (MTLTexture, [UInt8]) {
        let texture = try gpu.texture(q.format, 16, 8, usage: [.shaderRead], storage: .shared)
        let seeded = random(4_099, 16 * q.bytes * 8)
        try await gpu.upload(seeded, to: texture, bytesPerRow: 16 * q.bytes)
        return (texture, seeded)
    }

    static func blit(_ gpu: GPU, _ encode: (MTLBlitCommandEncoder) -> Void) async throws -> String {
        try await gpu.run(tolerateError: true) { commands in
            let blit = commands.makeBlitCommandEncoder()!
            encode(blit)
            blit.endEncoding()
        }.path
    }

    /// 8x4 texels from `from` of one texture to (1,1) of `to`
    static func copied(
        _ gpu: GPU, _ source: MTLTexture, _ to: MTLTexture, from: (x: Int, y: Int) = (2, 2)
    ) async throws -> String {
        try await blit(gpu) {
            $0.copy(
                from: source, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: from.x, y: from.y, z: 0),
                sourceSize: MTLSize(width: 8, height: 4, depth: 1), to: to, destinationSlice: 0, destinationLevel: 0,
                destinationOrigin: MTLOrigin(x: 1, y: 1, z: 0)
            )
        }
    }
}
