import Foundation
import HWTestCore
import Metal

/// The formats the 2D-engine probes run, with their channels and byte order
public struct X1Format: Sendable {
    public let name: String
    public let pixelFormat: MTLPixelFormat
    public let bytesPerTexel: Int
    public let channels: [X1Channel]

    public static let all: [X1Format] = [
        X1Format(
            name: "bgra8", pixelFormat: .bgra8Unorm, bytesPerTexel: 4,
            channels: .init(repeating: .unorm(bits: 8), count: 4)
        ),
        X1Format(
            name: "rgba8", pixelFormat: .rgba8Unorm, bytesPerTexel: 4,
            channels: .init(repeating: .unorm(bits: 8), count: 4)
        ),
        X1Format(
            name: "bgra8srgb", pixelFormat: .bgra8Unorm_srgb, bytesPerTexel: 4,
            channels: [.srgb8, .srgb8, .srgb8, .unorm(bits: 8)]
        ),
        X1Format(
            name: "rgba8srgb", pixelFormat: .rgba8Unorm_srgb, bytesPerTexel: 4,
            channels: [.srgb8, .srgb8, .srgb8, .unorm(bits: 8)]
        ),
        X1Format(
            name: "rgba16f", pixelFormat: .rgba16Float, bytesPerTexel: 8,
            channels: .init(repeating: .float(mantissa: 10), count: 4)
        ),
        X1Format(
            name: "rgb10a2", pixelFormat: .rgb10a2Unorm, bytesPerTexel: 4,
            channels: [.unorm(bits: 10), .unorm(bits: 10), .unorm(bits: 10), .unorm(bits: 2)]
        ),
        X1Format(
            name: "rg11b10f", pixelFormat: .rg11b10Float, bytesPerTexel: 4,
            channels: [.float(mantissa: 6), .float(mantissa: 6), .float(mantissa: 5)]
        ),
        X1Format(name: "r8", pixelFormat: .r8Unorm, bytesPerTexel: 1, channels: [.unorm(bits: 8)]),
        X1Format(name: "rg8", pixelFormat: .rg8Unorm, bytesPerTexel: 2, channels: [.unorm(bits: 8), .unorm(bits: 8)]),
        X1Format(
            name: "rgba32f", pixelFormat: .rgba32Float, bytesPerTexel: 16,
            channels: .init(repeating: .float32, count: 4)
        ),
    ]

    public static func named(_ names: [String]) -> [X1Format] { all.filter { names.contains($0.name) } }

    /// Texel t's channel codes, RGBA order
    public func codes(_ bytes: [UInt8], _ t: Int) -> [UInt32] {
        let at = t * bytesPerTexel
        func word(_ o: Int, _ n: Int) -> UInt32 { (0 ..< n).reduce(0) { $0 | UInt32(bytes[at + o + $1]) << (8 * $1) } }
        switch pixelFormat {
        case .bgra8Unorm, .bgra8Unorm_srgb: return [2, 1, 0, 3].map { UInt32(bytes[at + $0]) }
        case .rgba8Unorm, .rgba8Unorm_srgb, .r8Unorm, .rg8Unorm:
            return (0 ..< bytesPerTexel).map { UInt32(bytes[at + $0]) }
        case .rgba16Float: return (0 ..< 4).map { word($0 * 2, 2) }
        case .rgba32Float: return (0 ..< 4).map { word($0 * 4, 4) }
        case .rgb10a2Unorm:
            let w = word(0, 4)
            return [w & 1023, (w >> 10) & 1023, (w >> 20) & 1023, w >> 30]
        default:
            let w = word(0, 4)
            return [w & 2047, (w >> 11) & 2047, w >> 22]
        }
    }

    /// Texel t's channel codes, RGBA order, packed into SIMD4 without heap allocation.
    @inline(__always)
    public func codes4(_ base: UnsafePointer<UInt8>, _ t: Int) -> SIMD4<UInt32> {
        let p = base.advanced(by: t * bytesPerTexel)
        switch pixelFormat {
        case .bgra8Unorm, .bgra8Unorm_srgb:
            return SIMD4(UInt32(p[2]), UInt32(p[1]), UInt32(p[0]), UInt32(p[3]))
        case .rgba8Unorm, .rgba8Unorm_srgb:
            return SIMD4(UInt32(p[0]), UInt32(p[1]), UInt32(p[2]), UInt32(p[3]))
        case .r8Unorm:
            return SIMD4(UInt32(p[0]), 0, 0, 0)
        case .rg8Unorm:
            return SIMD4(UInt32(p[0]), UInt32(p[1]), 0, 0)
        case .rgba16Float:
            let raw = UnsafeRawPointer(p)
            return SIMD4(
                UInt32(raw.loadUnaligned(fromByteOffset: 0, as: UInt16.self).littleEndian),
                UInt32(raw.loadUnaligned(fromByteOffset: 2, as: UInt16.self).littleEndian),
                UInt32(raw.loadUnaligned(fromByteOffset: 4, as: UInt16.self).littleEndian),
                UInt32(raw.loadUnaligned(fromByteOffset: 6, as: UInt16.self).littleEndian)
            )
        case .rgba32Float:
            let raw = UnsafeRawPointer(p)
            return SIMD4(
                raw.loadUnaligned(fromByteOffset: 0, as: UInt32.self).littleEndian,
                raw.loadUnaligned(fromByteOffset: 4, as: UInt32.self).littleEndian,
                raw.loadUnaligned(fromByteOffset: 8, as: UInt32.self).littleEndian,
                raw.loadUnaligned(fromByteOffset: 12, as: UInt32.self).littleEndian
            )
        case .rgb10a2Unorm:
            let w = UnsafeRawPointer(p).loadUnaligned(as: UInt32.self).littleEndian
            return SIMD4(w & 1023, (w >> 10) & 1023, (w >> 20) & 1023, w >> 30)
        default:
            let w = UnsafeRawPointer(p).loadUnaligned(as: UInt32.self).littleEndian
            return SIMD4(w & 2047, (w >> 11) & 2047, w >> 22, 0)
        }
    }

    /// Writes one texel's RGBA codes directly into `out` at byte offset `offset`.
    @inline(__always)
    public func writeBytes(_ codes: SIMD4<UInt32>, into out: UnsafeMutableRawBufferPointer, at offset: Int) {
        switch pixelFormat {
        case .bgra8Unorm, .bgra8Unorm_srgb:
            out.storeBytes(of: UInt8(codes.z), toByteOffset: offset, as: UInt8.self)
            out.storeBytes(of: UInt8(codes.y), toByteOffset: offset + 1, as: UInt8.self)
            out.storeBytes(of: UInt8(codes.x), toByteOffset: offset + 2, as: UInt8.self)
            out.storeBytes(of: UInt8(codes.w), toByteOffset: offset + 3, as: UInt8.self)
        case .rgba8Unorm, .rgba8Unorm_srgb:
            out.storeBytes(of: UInt8(codes.x), toByteOffset: offset, as: UInt8.self)
            out.storeBytes(of: UInt8(codes.y), toByteOffset: offset + 1, as: UInt8.self)
            out.storeBytes(of: UInt8(codes.z), toByteOffset: offset + 2, as: UInt8.self)
            out.storeBytes(of: UInt8(codes.w), toByteOffset: offset + 3, as: UInt8.self)
        case .r8Unorm:
            out.storeBytes(of: UInt8(codes.x), toByteOffset: offset, as: UInt8.self)
        case .rg8Unorm:
            out.storeBytes(of: UInt8(codes.x), toByteOffset: offset, as: UInt8.self)
            out.storeBytes(of: UInt8(codes.y), toByteOffset: offset + 1, as: UInt8.self)
        case .rgba16Float:
            out.storeBytes(of: UInt16(codes.x).littleEndian, toByteOffset: offset, as: UInt16.self)
            out.storeBytes(of: UInt16(codes.y).littleEndian, toByteOffset: offset + 2, as: UInt16.self)
            out.storeBytes(of: UInt16(codes.z).littleEndian, toByteOffset: offset + 4, as: UInt16.self)
            out.storeBytes(of: UInt16(codes.w).littleEndian, toByteOffset: offset + 6, as: UInt16.self)
        case .rgba32Float:
            out.storeBytes(of: codes.x.littleEndian, toByteOffset: offset, as: UInt32.self)
            out.storeBytes(of: codes.y.littleEndian, toByteOffset: offset + 4, as: UInt32.self)
            out.storeBytes(of: codes.z.littleEndian, toByteOffset: offset + 8, as: UInt32.self)
            out.storeBytes(of: codes.w.littleEndian, toByteOffset: offset + 12, as: UInt32.self)
        case .rgb10a2Unorm:
            let w = codes.x | codes.y << 10 | codes.z << 20 | codes.w << 30
            out.storeBytes(of: w.littleEndian, toByteOffset: offset, as: UInt32.self)
        default:
            let w = codes.x | codes.y << 11 | codes.z << 22
            out.storeBytes(of: w.littleEndian, toByteOffset: offset, as: UInt32.self)
        }
    }

    /// The bytes of one texel from RGBA codes
    public func bytes(_ codes: [UInt32]) -> [UInt8] {
        func le(_ v: UInt32, _ n: Int) -> [UInt8] { (0 ..< n).map { UInt8(truncatingIfNeeded: v >> (8 * $0)) } }
        switch pixelFormat {
        case .bgra8Unorm, .bgra8Unorm_srgb: return [2, 1, 0, 3].map { UInt8(codes[$0]) }
        case .rgba8Unorm, .rgba8Unorm_srgb, .r8Unorm, .rg8Unorm: return codes.map { UInt8($0) }
        case .rgba16Float: return codes.flatMap { le($0, 2) }
        case .rgba32Float: return codes.flatMap { le($0, 4) }
        case .rgb10a2Unorm: return le(codes[0] | codes[1] << 10 | codes[2] << 20 | codes[3] << 30, 4)
        default: return le(codes[0] | codes[1] << 11 | codes[2] << 22, 4)
        }
    }
}
