import Foundation
import HWTestCore
import Metal

/// generateMipmaps on power-of-two textures, every level against a 2x2 box filter of the level above as stored (in
/// linear light for sRGB), and on a non-power-of-two one of a single colour, whose levels must all keep it. On the raw
/// facade every generateMipmaps stays raw, on the 2D engine's formats and the 3D engine's mip states alike.
struct MipmapScene: GPUScene {
    let id = "gpu.mipmaps"
    let name = "Mipmap generation, every level"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        var paths: [String] = []
        let formats: [(TexelFormat, Int)] = [
            (.rgba8Unorm, 256), (.r8Unorm, 128), (.rgba16Float, 64), (.bgra8Srgb, 64), (.rgba8Srgb, 64),
            (.r16Float, 64), (.rg16Float, 32),
        ]
        for (format, size) in formats {
            let levels = Int(log2(Double(size))) + 1
            let texture = try gpu.texture(
                format.metal, size, size, usage: [.shaderRead, .renderTarget], mipmaps: levels)
            let bytes = Patterns.bytes(Patterns.busy(width: size, height: size, channels: format.channels), format)
            let base = format.raster(bytes, width: size, height: size)
            try await gpu.blitUpload(
                [UploadRect(x: 0, y: 0, width: size, height: size)], [bytes], into: texture, texel: format.bytesPerTexel
            )
            let generated = try await gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.generateMipmaps(for: texture)
                blit.endEncoding()
            }
            paths.append("\(format.rawValue):\(generated.path)")
            var reference = base
            for level in 1..<levels {
                reference = Self.halved(reference, format)
                let got = try await gpu.raster(texture, format, level: level)
                result.compare(
                    "\(format.rawValue) \(size) level \(level)", got, reference, tolerance: 2.01 * format.step())
            }
        }
        let (width, height) = (100, 60)
        let odd = try gpu.texture(.rgba8Unorm, width, height, usage: [.shaderRead, .renderTarget], mipmaps: 7)
        let color: [Float] = [0.2, 0.4, 0.6, 1]
        try await gpu.blitUpload(
            [UploadRect(x: 0, y: 0, width: width, height: height)],
            [Patterns.repeated(TexelFormat.rgba8Unorm.encode(color), count: width * height)], into: odd, texel: 4)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.generateMipmaps(for: odd)
            blit.endEncoding()
        }
        for level in 1..<7 {
            let got = try await gpu.raster(odd, .rgba8Unorm, level: level)
            let want = Raster(width: got.width, height: got.height, channels: 4) { _, _, c in color[c] }
            result.compare(
                "rgba8Unorm 100x60 level \(level)", got, TexelFormat.rgba8Unorm.quantized(want), tolerance: 1.01 / 255)
        }
        result.metrics.set("paths", paths.joined(separator: ","))
        if RawFacade.kind(of: gpu.device) == "raw" {
            let off = paths.filter { !$0.hasSuffix(":raw") }
            result.expect(off.isEmpty, "generateMipmaps off the raw path: \(off.joined(separator: ","))")
        }
        return result
    }

    static func halved(_ image: Raster, _ format: TexelFormat) -> Raster {
        let srgb = format == .bgra8Srgb || format == .rgba8Srgb
        let half = Raster(
            width: max(1, image.width / 2), height: max(1, image.height / 2), channels: image.channels,
            levels: image.levels
        ) { x, y, c in
            let texels = [(0, 0), (1, 0), (0, 1), (1, 1)].map { image[x * 2 + $0.0, y * 2 + $0.1, c] }
            let linear = srgb && c < 3 ? texels.map(TexelFormat.linear) : texels
            return linear.reduce(0, +) / 4
        }
        return format.quantized(half)
    }
}
