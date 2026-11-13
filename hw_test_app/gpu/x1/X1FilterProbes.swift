import Foundation
import HWTestCore
import Metal

/// METALMAP d7 and LIMITS G13: the texture unit's 32-bit float filtering, and the 2D engine's mip downsample (the raw
/// path's generateMipmaps for BGRA8, RGBA8, RGBA16F, R8 and RG8) against an exact 2x2 box. An M-series Mac's own
/// downsample is off the box by up to one code (X1ProbeTests), so only a code further off counts against it.
public enum X1FilterProbes {
    public static func f32Filter(_ gpu: GPU) async throws -> ProbeFinding {
        let texture = try gpu.texture(.rgba32Float, 2, 1, usage: [.shaderRead])
        let texels: [Float] = [0, 0, 0, 0, 1e6, 1e6, 1e6, 1e6]
        try await gpu.upload(texels.withUnsafeBytes { Array($0) }, to: texture, bytesPerRow: 32)
        let out = try gpu.buffer(16)
        let kernel = try gpu.compute("x1_sample_linear")
        let path = try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(kernel)
            encoder.setTexture(texture, index: 0)
            var uv = SIMD2<Float>(0.375, 0.5)
            encoder.setBytes(&uv, length: 8, index: 0)
            encoder.setBuffer(out, offset: 0, index: 1)
            encoder.dispatchThreads(
                MTLSize(width: 1, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
            encoder.endEncoding()
        }.path
        let value = out.contents().load(as: Float.self)
        var metrics = Metrics()
        metrics.set("value", Double(value), digits: 3)
        metrics.set("supports_32bit_filtering", gpu.device.supports32BitFloatFiltering)
        let off = abs(Double(value) - 250_000) > Double(Float(250_000).ulp)
        return ProbeFinding.counted(["value_wrong": off ? 1 : 0], paths: [path], metrics: metrics)
    }

    struct MipFormat {
        let name: String
        let format: MTLPixelFormat
        let channels: Int
        let half: Bool
    }

    static let mipFormats = [
        MipFormat(name: "bgra8", format: .bgra8Unorm, channels: 4, half: false),
        MipFormat(name: "rgba8", format: .rgba8Unorm, channels: 4, half: false),
        MipFormat(name: "rgba16f", format: .rgba16Float, channels: 4, half: true),
        MipFormat(name: "r8", format: .r8Unorm, channels: 1, half: false),
        MipFormat(name: "rg8", format: .rg8Unorm, channels: 2, half: false),
    ]

    /// A code of 255 per channel: a diagonal gradient, a 4-texel checker, a product and a ramp.
    static func code(_ x: Int, _ y: Int, _ c: Int) -> Int {
        switch c {
        case 0: return (x * 5 + y * 3) & 255
        case 1: return ((x >> 2) ^ (y >> 2)) & 1 == 1 ? 255 : 0
        case 2: return (x * y) & 255
        default: return 255 - x
        }
    }

    public static func mipFilter(_ gpu: GPU) async throws -> ProbeFinding {
        let size = 256
        var wrong: [String: Int] = [:]
        var metrics = Metrics()
        var paths: [String] = []
        for f in mipFormats {
            let texture = try gpu.texture(f.format, size, size, usage: [.shaderRead, .renderTarget], mipmaps: 2)
            let unit = f.half ? 2 : 1
            var bytes = [UInt8](repeating: 0, count: size * size * f.channels * unit)
            for y in 0..<size {
                for x in 0..<size {
                    for c in 0..<f.channels {
                        let at = ((y * size + x) * f.channels + c) * unit
                        let value = code(x, y, c)
                        if f.half {
                            let bits = Float16(Double(value) / 255).bitPattern
                            bytes[at] = UInt8(bits & 0xff)
                            bytes[at + 1] = UInt8(bits >> 8)
                        } else {
                            bytes[at] = UInt8(value)
                        }
                    }
                }
            }
            try await gpu.upload(bytes, to: texture, bytesPerRow: size * f.channels * unit)
            paths.append(
                try await gpu.run { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.generateMipmaps(for: texture)
                    blit.endEncoding()
                }.path)
            let level = try await gpu.read(texture, bytesPerPixel: f.channels * unit, level: 1)
            var exact = 0
            var beyondOne = 0
            var maxDiff = 0.0
            let half = size / 2
            for y in 0..<half {
                for x in 0..<half {
                    for c in 0..<f.channels {
                        let quad = [(0, 0), (1, 0), (0, 1), (1, 1)].map { code(2 * x + $0.0, 2 * y + $0.1, c) }
                        let sum = quad.reduce(0, +)
                        let at = ((y * half + x) * f.channels + c) * unit
                        let have: Double
                        let want: Double
                        if f.half {
                            let bits = UInt16(level[at]) | UInt16(level[at + 1]) << 8
                            have = Double(Float16(bitPattern: bits)) * 255
                            let parts = quad.map { Double(Float16(Double($0) / 255)) }
                            want = Double(Float16(parts.reduce(0, +) / 4)) * 255
                        } else {
                            have = Double(level[at])
                            want = (Double(sum) / 4).rounded(.toNearestOrAwayFromZero)
                        }
                        let diff = abs(have - want)
                        if diff > 0 { exact += 1 }
                        if diff > 1 { beyondOne += 1 }
                        maxDiff = max(maxDiff, diff)
                    }
                }
            }
            wrong[f.name + "_wrong"] = beyondOne
            metrics.set(f.name + "_inexact", exact)
            metrics.set(f.name + "_max_diff", maxDiff, digits: 3)
        }
        return ProbeFinding.counted(wrong, paths: paths, metrics: metrics)
    }
}
