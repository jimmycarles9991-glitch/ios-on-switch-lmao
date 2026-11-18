import Foundation
import HWTestCore
import Metal

extension TexelFormat {
    public var metal: MTLPixelFormat {
        switch self {
        case .rgba8Unorm: return .rgba8Unorm
        case .bgra8Unorm: return .bgra8Unorm
        case .rgba8Srgb: return .rgba8Unorm_srgb
        case .bgra8Srgb: return .bgra8Unorm_srgb
        case .r8Unorm: return .r8Unorm
        case .a8Unorm: return .a8Unorm
        case .rg8Unorm: return .rg8Unorm
        case .rgba16Float: return .rgba16Float
        case .r16Float: return .r16Float
        case .rg16Float: return .rg16Float
        case .rgba32Float: return .rgba32Float
        case .r32Float: return .r32Float
        case .rgb10a2Unorm: return .rgb10a2Unorm
        case .bgr10a2Unorm: return .bgr10a2Unorm
        case .bgr10XR: return .bgr10_xr
        case .bgra10XR: return .bgra10_xr
        case .depth32Float: return .depth32Float
        case .depth16Unorm: return .depth16Unorm
        case .stencil8: return .stencil8
        }
    }

    public var isDepthOrStencil: Bool { self == .depth32Float || self == .depth16Unorm || self == .stencil8 }

    /// Whether `device` makes a texture of this format with `usage` (the extended-range formats are Apple-family
    /// only; the Switch's m2v device answers for itself).
    public func supported(on device: MTLDevice, usage: MTLTextureUsage) -> Bool {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: metal, width: 8, height: 8, mipmapped: false)
        descriptor.usage = usage
        descriptor.storageMode = .private
        return device.makeTexture(descriptor: descriptor) != nil
    }
}

/// Deterministic test pictures: every value a multiple of 1/255, so 8-bit formats hold them exactly.
public enum Patterns {
    /// Gradients, a checker and a diagonal, different in every channel and every 8x8 block.
    public static func busy(width: Int, height: Int, channels: Int, seed: Int = 0) -> Raster {
        Raster(width: width, height: height, channels: channels) { x, y, c in
            let checker = ((x >> 3) ^ (y >> 3) ^ seed) & 1
            let value: Int
            switch (c + seed) % 4 {
            case 0: value = (x * 255) / max(1, width - 1)
            case 1: value = (y * 255) / max(1, height - 1)
            case 2: value = checker == 1 ? 200 : 40
            default: value = ((x + y * 3 + seed * 17) * 37) & 255
            }
            return Float(c == 3 && channels == 4 ? max(value, 1) : value) / 255
        }
    }

    public static func opaque(_ image: Raster) -> Raster {
        var out = image
        for y in 0..<image.height { for x in 0..<image.width { out[x, y, 3] = 1 } }
        return out
    }

    /// A glyph-like coverage bitmap: soft round edges, a stem and a hole, different for every `index`.
    public static func glyph(width: Int, height: Int, index: Int) -> Raster {
        Raster(width: width, height: height, channels: 1) { x, y, _ in
            let (cx, cy) = (Float(width) / 2, Float(height) / 2)
            let (dx, dy) = ((Float(x) + 0.5 - cx) / cx, (Float(y) + 0.5 - cy) / cy)
            let ring = abs(sqrt(dx * dx + dy * dy) - 0.6)
            let stem = abs(Float(x) + 0.5 - Float(index % max(1, width)))
            let coverage = max(0, 1 - ring * 3, 1 - stem / 1.5)
            return Float(Int(min(coverage, 1) * 255) ^ (index & 7)) / 255
        }
    }

    /// One texel's bytes `count` times over.
    public static func repeated(_ texel: [UInt8], count: Int) -> [UInt8] {
        [UInt8](unsafeUninitializedCapacity: texel.count * count) { buffer, initialized in
            for i in 0..<(texel.count * count) { buffer[i] = texel[i % texel.count] }
            initialized = texel.count * count
        }
    }

    /// The raster's bytes in `format` (rows packed).
    public static func bytes(_ raster: Raster, _ format: TexelFormat) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(raster.width * raster.height * format.bytesPerTexel)
        for y in 0..<raster.height {
            for x in 0..<raster.width { out += format.encode(raster.pixel(x, y)) }
        }
        return out
    }
}
