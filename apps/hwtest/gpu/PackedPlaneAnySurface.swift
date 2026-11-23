import CoreVideo
import Foundation
import HWTestCore
import IOSurface
import Metal

/// R10Unorm_PACKED (588) and RG10Unorm_PACKED (589) over planes of any surface code, size and plane count, every word
/// of the surface a seeded xorshift first, read by a kernel (gpu.gm20b.packed_plane_any_surface). An M4 Max reads channel
/// c < k (k 1 for 588, 2 for 589) of texel (x, y) as code / 1023, the code the 10 bits at bit 10 * (n % 3) of the word
/// at (off & ~15) + (rows & ~15) * y + 4 * (n / 3), n = k * x + c (off, rows: the plane's offset and bytes a row, a
/// planeless surface's 0 and its rows), 0 where that word lies past the surface's last page; other channels 0, 0, 1.
public enum PackedPlaneAnySurface {
    public struct Scene: Sendable {
        public let key: String
        let format: UInt
        let plane: Int
        let width, height: Int
        let make: @Sendable () -> (IOSurfaceRef?, CVPixelBuffer?)
    }

    /// One plane of IOSurfaceCreate's plane info: width, height, bytes and pixels an element, rows, offset, size
    struct Plane {
        let width, height, bytes, pixels, rows, offset, size: Int
    }

    static let p420: OSType = 0x7034_3230
    /// Three 10-bit codes an element: luma in elements of 4 bytes, chroma pairs in elements of 8
    static func codes(_ w: Int, _ h: Int, rows: Int, at: Int, bytes: Int) -> Plane {
        Plane(width: w, height: h, bytes: bytes, pixels: 3, rows: rows, offset: at, size: rows * h)
    }
    static func luma(_ w: Int, _ h: Int, _ rows: Int, _ at: Int) -> Plane { codes(w, h, rows: rows, at: at, bytes: 4) }
    static func chroma(_ w: Int, _ h: Int, _ rows: Int, _ at: Int) -> Plane {
        codes(w, h, rows: rows, at: at, bytes: 8)
    }
    static let yuv420 = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange

    public static let scenes: [Scene] = [
        Scene(key: "420v_luma", format: 588, plane: 0, width: 32, height: 16, make: { video(yuv420) }),
        Scene(key: "420v_chroma", format: 589, plane: 1, width: 16, height: 8, make: { video(yuv420) }),
        Scene(key: "bgra_588", format: 588, plane: 0, width: 32, height: 16, make: { (planeless(), nil) }),
        Scene(key: "bgra_589", format: 589, plane: 0, width: 32, height: 16, make: { (planeless(), nil) }),
        Scene(key: "p420_588_plane1", format: 588, plane: 1, width: 16, height: 8, make: { video(p420) }),
        Scene(key: "p420_589_plane0", format: 589, plane: 0, width: 32, height: 16, make: { video(p420) }),
        Scene(key: "p420_588_8x8", format: 588, plane: 0, width: 8, height: 8, make: { video(p420) }),
        Scene(key: "rows88_588", format: 588, plane: 0, width: 64, height: 16, make: { (rows88(), nil) }),
        Scene(key: "rows88_589", format: 589, plane: 1, width: 32, height: 8, make: { (rows88(), nil) }),
        Scene(
            key: "plane1_at1544", format: 589, plane: 1, width: 32, height: 8,
            make: { (planar(64, 16, [luma(64, 16, 96, 0), chroma(32, 8, 96, 1544)], 2320), nil) }
        ),
        Scene(
            key: "past_mapping", format: 589, plane: 1, width: 16, height: 8,
            make: {
                let pairs = Plane(width: 16, height: 8, bytes: 2, pixels: 1, rows: 64, offset: 15904, size: 480)
                return (planar(32, 16, [luma(32, 16, 64, 0), pairs], 16384), nil)
            }
        ),
    ]

    static func rows88() -> IOSurfaceRef? { planar(64, 16, [luma(64, 16, 88, 0), chroma(32, 8, 88, 1408)], 2112) }

    static func planar(_ w: Int, _ h: Int, _ planes: [Plane], _ allocation: Int) -> IOSurfaceRef? {
        let info = planes.map { p -> [CFString: Any] in
            [
                kIOSurfacePlaneWidth: p.width, kIOSurfacePlaneHeight: p.height, kIOSurfacePlaneBytesPerElement: p.bytes,
                kIOSurfacePlaneElementWidth: p.pixels, kIOSurfacePlaneBytesPerRow: p.rows,
                kIOSurfacePlaneOffset: p.offset, kIOSurfacePlaneSize: p.size,
            ]
        }
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: w, kIOSurfaceHeight: h, kIOSurfacePixelFormat: p420, kIOSurfaceAllocSize: allocation,
            kIOSurfacePlaneInfo: info,
        ]
        return IOSurfaceCreate(properties as CFDictionary)
    }

    static func planeless() -> IOSurfaceRef? {
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: 32, kIOSurfaceHeight: 16, kIOSurfacePixelFormat: kCVPixelFormatType_32BGRA,
            kIOSurfaceBytesPerElement: 4, kIOSurfaceBytesPerRow: 128, kIOSurfaceAllocSize: 2048,
        ]
        return IOSurfaceCreate(properties as CFDictionary)
    }

    static func video(_ fourcc: OSType) -> (IOSurfaceRef?, CVPixelBuffer?) {
        var made: CVPixelBuffer?
        let attributes =
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        guard CVPixelBufferCreate(nil, 32, 16, fourcc, attributes, &made) == kCVReturnSuccess, let buffer = made else {
            return (nil, nil)
        }
        return (CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(), buffer)
    }

    public struct Reading: Sendable {
        public var made = false
        /// Texels with a channel off its code by half a code or more, or off its constant
        public var off = 0
        /// The largest distance of a read channel from its code, in codes
        public var worst: Float = 0
        public var first = "-"
    }

    /// Every word of the allocation from xorshift32 seeded `seed`
    static func seed(_ surface: IOSurfaceRef, _ seed: UInt32) {
        IOSurfaceLock(surface, [], nil)
        let count = IOSurfaceGetAllocSize(surface) / 4
        let words = IOSurfaceGetBaseAddress(surface).bindMemory(to: UInt32.self, capacity: count)
        var x = seed
        for i in 0 ..< count {
            x ^= x << 13
            x ^= x >> 17
            x ^= x << 5
            words[i] = x
        }
        IOSurfaceUnlock(surface, [], nil)
    }

    public static func observe(_ gpu: GPU, _ s: Scene) async throws -> Reading {
        let (made, owner) = s.make()
        guard let surface = made else { throw TestFailure("no surface for \(s.key)") }
        defer { withExtendedLifetime(owner) {} }
        guard let texture = SurfaceReads.texture(gpu, s.format, s.width, s.height, over: surface, plane: s.plane) else {
            return Reading()
        }
        seed(surface, 0x9e37_79b9 ^ UInt32(s.key.utf8.count))
        let bytes = SurfaceReads.bytes(surface)
        let page = Int(vm_page_size), mapped = (IOSurfaceGetAllocSize(surface) + page - 1) / page * page
        let planar = IOSurfaceGetPlaneCount(surface) > 0
        let base = Int(bitPattern: IOSurfaceGetBaseAddress(surface))
        let offset = planar ? Int(bitPattern: IOSurfaceGetBaseAddressOfPlane(surface, s.plane)) - base : 0
        let rows = planar ? IOSurfaceGetBytesPerRowOfPlane(surface, s.plane) : IOSurfaceGetBytesPerRow(surface)
        let k = s.format == 589 ? 2 : 1
        let code = { (x: Int, y: Int, c: Int) -> Float in
            let n = k * x + c, at = (offset & ~15) + (rows & ~15) * y + 4 * (n / 3)
            guard at + 4 <= min(mapped, bytes.count) else { return 0 }
            return Float((SurfaceReads.word(bytes, at) >> UInt32(10 * (n % 3))) & 1023)
        }
        let got = try await SurfaceReads.read(gpu, texture)
        var reading = Reading(made: true)
        for (i, texel) in got.enumerated() {
            let (x, y) = (i % s.width, i / s.width)
            var wrong = false
            for c in 0 ..< 4 {
                if c < k {
                    let distance = abs(texel[c] * 1023 - code(x, y, c))
                    reading.worst = max(reading.worst, distance)
                    wrong = wrong || !(distance < 0.5)
                } else {
                    wrong = wrong || abs(texel[c] - (c == 3 ? 1 : 0)) > 1e-6
                }
            }
            guard wrong else { continue }
            reading.off += 1
            if reading.first == "-" { reading.first = "(\(x),\(y))=\(texel)" }
        }
        return reading
    }
}
