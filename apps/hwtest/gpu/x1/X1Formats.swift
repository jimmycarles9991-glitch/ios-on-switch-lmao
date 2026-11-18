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
            channels: .init(repeating: .unorm(bits: 8), count: 4)),
        X1Format(
            name: "rgba8", pixelFormat: .rgba8Unorm, bytesPerTexel: 4,
            channels: .init(repeating: .unorm(bits: 8), count: 4)),
        X1Format(
            name: "bgra8srgb", pixelFormat: .bgra8Unorm_srgb, bytesPerTexel: 4,
            channels: [.srgb8, .srgb8, .srgb8, .unorm(bits: 8)]),
        X1Format(
            name: "rgba8srgb", pixelFormat: .rgba8Unorm_srgb, bytesPerTexel: 4,
            channels: [.srgb8, .srgb8, .srgb8, .unorm(bits: 8)]),
        X1Format(
            name: "rgba16f", pixelFormat: .rgba16Float, bytesPerTexel: 8,
            channels: .init(repeating: .float(mantissa: 10), count: 4)),
        X1Format(
            name: "rgb10a2", pixelFormat: .rgb10a2Unorm, bytesPerTexel: 4,
            channels: [.unorm(bits: 10), .unorm(bits: 10), .unorm(bits: 10), .unorm(bits: 2)]),
        X1Format(
            name: "rg11b10f", pixelFormat: .rg11b10Float, bytesPerTexel: 4,
            channels: [.float(mantissa: 6), .float(mantissa: 6), .float(mantissa: 5)]),
        X1Format(name: "r8", pixelFormat: .r8Unorm, bytesPerTexel: 1, channels: [.unorm(bits: 8)]),
        X1Format(name: "rg8", pixelFormat: .rg8Unorm, bytesPerTexel: 2, channels: [.unorm(bits: 8), .unorm(bits: 8)]),
        X1Format(
            name: "rgba32f", pixelFormat: .rgba32Float, bytesPerTexel: 16,
            channels: .init(repeating: .float32, count: 4)),
    ]

    public static func named(_ names: [String]) -> [X1Format] { all.filter { names.contains($0.name) } }

    /// Texel t's channel codes, RGBA order
    public func codes(_ bytes: [UInt8], _ t: Int) -> [UInt32] {
        let at = t * bytesPerTexel
        func word(_ o: Int, _ n: Int) -> UInt32 { (0..<n).reduce(0) { $0 | UInt32(bytes[at + o + $1]) << (8 * $1) } }
        switch pixelFormat {
        case .bgra8Unorm, .bgra8Unorm_srgb: return [2, 1, 0, 3].map { UInt32(bytes[at + $0]) }
        case .rgba8Unorm, .rgba8Unorm_srgb, .r8Unorm, .rg8Unorm:
            return (0..<bytesPerTexel).map { UInt32(bytes[at + $0]) }
        case .rgba16Float: return (0..<4).map { word($0 * 2, 2) }
        case .rgba32Float: return (0..<4).map { word($0 * 4, 4) }
        case .rgb10a2Unorm:
            let w = word(0, 4)
            return [w & 1023, (w >> 10) & 1023, (w >> 20) & 1023, w >> 30]
        default:
            let w = word(0, 4)
            return [w & 2047, (w >> 11) & 2047, w >> 22]
        }
    }

    /// The bytes of one texel from RGBA codes
    public func bytes(_ codes: [UInt32]) -> [UInt8] {
        func le(_ v: UInt32, _ n: Int) -> [UInt8] { (0..<n).map { UInt8(truncatingIfNeeded: v >> (8 * $0)) } }
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
