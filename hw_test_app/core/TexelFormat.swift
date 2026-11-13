import Foundation

/// The texel formats the GPU scenes write and read back, with their bytes as a CPU stores and reads them. Raster
/// channels are always in RGBA order (A8 holds only alpha, depth and stencil one value); `encode` and `decode` take
/// and give them in that order whatever the memory order is. `encode` takes linear values; sRGB formats `decode` to
/// their stored (encoded) values, so they are compared where one step is one level. BGRA10_XR's alpha is XR-encoded
/// but holds [0, 1] only, as Apple's GPUs store it.
public enum TexelFormat: String, CaseIterable, Sendable {
    case rgba8Unorm, bgra8Unorm, rgba8Srgb, bgra8Srgb, r8Unorm, a8Unorm, rg8Unorm
    case rgba16Float, r16Float, rg16Float, rgba32Float, r32Float
    case rgb10a2Unorm, bgr10a2Unorm, bgr10XR, bgra10XR
    case depth32Float, depth16Unorm, stencil8

    public var bytesPerTexel: Int {
        switch self {
        case .r8Unorm, .a8Unorm, .stencil8: return 1
        case .rg8Unorm, .r16Float, .depth16Unorm: return 2
        case .rgba8Unorm, .bgra8Unorm, .rgba8Srgb, .bgra8Srgb, .rg16Float, .r32Float, .rgb10a2Unorm, .bgr10a2Unorm,
            .bgr10XR, .depth32Float:
            return 4
        case .rgba16Float, .bgra10XR: return 8
        case .rgba32Float: return 16
        }
    }

    public var channels: Int {
        switch self {
        case .r8Unorm, .a8Unorm, .r16Float, .r32Float, .depth32Float, .depth16Unorm, .stencil8: return 1
        case .rg8Unorm, .rg16Float: return 2
        case .bgr10XR: return 3
        default: return 4
        }
    }

    public var isFloat: Bool {
        switch self {
        case .rgba16Float, .r16Float, .rg16Float, .rgba32Float, .r32Float, .depth32Float: return true
        default: return false
        }
    }

    /// The log's unit: 8-bit formats in levels, the others as values.
    public var levels: Float {
        switch self {
        case .rgba8Unorm, .bgra8Unorm, .rgba8Srgb, .bgra8Srgb, .r8Unorm, .a8Unorm, .rg8Unorm: return 255
        default: return 1
        }
    }

    /// One step of the format near `value`: the tolerance a conversion that rounds either way needs.
    public func step(near value: Float = 1) -> Float {
        switch self {
        case .rgba8Unorm, .bgra8Unorm, .rgba8Srgb, .bgra8Srgb, .r8Unorm, .a8Unorm, .rg8Unorm: return 1 / 255
        case .rgb10a2Unorm, .bgr10a2Unorm: return 1 / 1023
        case .bgr10XR, .bgra10XR: return 1 / 510
        case .depth16Unorm: return 1 / 65535
        case .stencil8: return 0
        case .rgba16Float, .r16Float, .rg16Float: return max(abs(value), 1.0 / 16384) / 1024
        case .rgba32Float, .r32Float, .depth32Float: return max(abs(value), 1e-30) / 8_388_608
        }
    }

    /// The bytes an exact GPU stores for `rgba` (values in the raster's channel order).
    public func encode(_ value: [Float]) -> [UInt8] {
        precondition(value.count == channels, "\(rawValue) takes \(channels) channels")
        switch self {
        case .rgba8Unorm, .r8Unorm, .a8Unorm, .rg8Unorm: return value.map(Self.unorm8)
        case .bgra8Unorm: return [value[2], value[1], value[0], value[3]].map(Self.unorm8)
        case .rgba8Srgb:
            return [Self.srgb(value[0]), Self.srgb(value[1]), Self.srgb(value[2]), value[3]].map(Self.unorm8)
        case .bgra8Srgb:
            return [Self.srgb(value[2]), Self.srgb(value[1]), Self.srgb(value[0]), value[3]].map(Self.unorm8)
        case .rgba16Float, .r16Float, .rg16Float: return value.flatMap { Self.bytes(Float16($0).bitPattern) }
        case .rgba32Float, .r32Float, .depth32Float: return value.flatMap { Self.bytes($0.bitPattern) }
        case .depth16Unorm: return Self.bytes(UInt16((min(max(value[0], 0), 1) * 65535).rounded()))
        case .stencil8: return [UInt8(min(max(value[0], 0), 255))]
        case .rgb10a2Unorm:
            let (r, g, b) = (Self.unorm(value[0], 1023), Self.unorm(value[1], 1023), Self.unorm(value[2], 1023))
            return Self.bytes(r | g << 10 | b << 20 | Self.unorm(value[3], 3) << 30)
        case .bgr10a2Unorm:
            let (r, g, b) = (Self.unorm(value[0], 1023), Self.unorm(value[1], 1023), Self.unorm(value[2], 1023))
            return Self.bytes(b | g << 10 | r << 20 | Self.unorm(value[3], 3) << 30)
        case .bgr10XR:
            return Self.bytes(Self.xr(value[2]) | Self.xr(value[1]) << 10 | Self.xr(value[0]) << 20)
        case .bgra10XR:
            let alpha = min(max(value[3], 0), 1)
            return [value[2], value[1], value[0], alpha].flatMap { Self.bytes(UInt16(Self.xr($0) << 6)) }
        }
    }

    public func decode(_ bytes: ArraySlice<UInt8>) -> [Float] {
        let b = Array(bytes)
        precondition(b.count == bytesPerTexel, "\(rawValue) texels are \(bytesPerTexel) bytes")
        switch self {
        case .rgba8Unorm, .rgba8Srgb, .r8Unorm, .a8Unorm, .rg8Unorm: return b.map { Float($0) / 255 }
        case .bgra8Unorm, .bgra8Srgb: return [b[2], b[1], b[0], b[3]].map { Float($0) / 255 }
        case .rgba16Float, .r16Float, .rg16Float:
            return stride(from: 0, to: b.count, by: 2).map { Float(Float16(bitPattern: UInt16(Self.word(b, $0, 2)))) }
        case .rgba32Float, .r32Float, .depth32Float:
            return stride(from: 0, to: b.count, by: 4).map { Float(bitPattern: Self.word(b, $0, 4)) }
        case .depth16Unorm: return [Float(Self.word(b, 0, 2)) / 65535]
        case .stencil8: return [Float(b[0])]
        case .rgb10a2Unorm:
            let w = Self.word(b, 0, 4)
            return [w & 1023, w >> 10 & 1023, w >> 20 & 1023].map { Float($0) / 1023 } + [Float(w >> 30) / 3]
        case .bgr10a2Unorm:
            let w = Self.word(b, 0, 4)
            return [w >> 20 & 1023, w >> 10 & 1023, w & 1023].map { Float($0) / 1023 } + [Float(w >> 30) / 3]
        case .bgr10XR:
            let w = Self.word(b, 0, 4)
            return [w >> 20 & 1023, w >> 10 & 1023, w & 1023].map(Self.xrValue)
        case .bgra10XR:
            let words = stride(from: 0, to: 8, by: 2).map { Self.word(b, $0, 2) >> 6 }
            return [words[2], words[1], words[0], words[3]].map(Self.xrValue)
        }
    }

    /// A whole read-back image (rows packed) as a raster.
    public func raster(_ bytes: [UInt8], width: Int, height: Int) -> Raster {
        let count = width * height * bytesPerTexel
        precondition(bytes.count >= count, "\(rawValue) \(width)x\(height) needs \(count) bytes")
        switch self {
        case .rgba8Unorm, .rgba8Srgb, .r8Unorm, .a8Unorm, .rg8Unorm:
            return Raster(
                width: width, height: height, channels: channels, levels: levels,
                samples: bytes[0..<count].map { Float($0) / 255 })
        case .rgba32Float, .r32Float, .depth32Float:
            let samples = bytes.withUnsafeBytes { raw in
                (0..<(count / 4)).map { raw.loadUnaligned(fromByteOffset: $0 * 4, as: Float.self) }
            }
            return Raster(width: width, height: height, channels: channels, levels: levels, samples: samples)
        default:
            break
        }
        var samples: [Float] = []
        samples.reserveCapacity(width * height * channels)
        for at in stride(from: 0, to: width * height * bytesPerTexel, by: bytesPerTexel) {
            samples += decode(bytes[at..<(at + bytesPerTexel)])
        }
        return Raster(width: width, height: height, channels: channels, levels: levels, samples: samples)
    }

    /// What an exact GPU would read back after storing `raster`: each texel through this format and back.
    public func quantized(_ raster: Raster) -> Raster {
        var out = raster
        for y in 0..<raster.height {
            for x in 0..<raster.width {
                let stored = decode(encode(raster.pixel(x, y))[...])
                for c in 0..<channels { out[x, y, c] = stored[c] }
            }
        }
        return Raster(width: out.width, height: out.height, channels: channels, levels: levels, samples: out.samples)
    }

    static func unorm8(_ value: Float) -> UInt8 { UInt8(unorm(value, 255)) }

    static func unorm(_ value: Float, _ top: UInt32) -> UInt32 {
        value.isNaN ? 0 : UInt32((min(max(value, 0), 1) * Float(top)).rounded())
    }

    static func xr(_ value: Float) -> UInt32 { UInt32(min(max((value * 510 + 384).rounded(), 0), 1023)) }

    static func xrValue(_ word: UInt32) -> Float { (Float(word) - 384) / 510 }

    public static func srgb(_ linear: Float) -> Float {
        let v = min(max(linear, 0), 1)
        return v <= 0.003_130_8 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055
    }

    public static func linear(_ srgb: Float) -> Float {
        srgb <= 0.040_45 ? srgb / 12.92 : pow((srgb + 0.055) / 1.055, 2.4)
    }

    static func bytes<T: FixedWidthInteger>(_ value: T) -> [UInt8] {
        (0..<(T.bitWidth / 8)).map { UInt8(truncatingIfNeeded: value >> ($0 * 8)) }
    }

    static func word(_ bytes: [UInt8], _ at: Int, _ count: Int) -> UInt32 {
        (0..<count).reduce(UInt32(0)) { $0 | UInt32(bytes[at + $1]) << ($1 * 8) }
    }
}
