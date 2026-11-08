import Foundation
import HWTestCore
import Metal

/// generateMipmaps on power-of-two textures, every level against a 2x2 box filter of the level above as stored (in
/// linear light for sRGB), and on a non-power-of-two one of a single colour, whose levels must all keep it.
struct MipmapScene: GPUScene {
    let id = "gpu.mipmaps"
    let name = "Mipmap generation, every level"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        for (format, size) in [(TexelFormat.rgba8Unorm, 256), (.r8Unorm, 128), (.rgba16Float, 64), (.bgra8Srgb, 64)] {
            let levels = Int(log2(Double(size))) + 1
            let texture = try gpu.texture(
                format.metal, size, size, usage: [.shaderRead, .renderTarget], mipmaps: levels)
            let bytes = Patterns.bytes(Patterns.busy(width: size, height: size, channels: format.channels), format)
            let base = format.raster(bytes, width: size, height: size)
            try await gpu.blitUpload(
                [UploadRect(x: 0, y: 0, width: size, height: size)], [bytes], into: texture, texel: format.bytesPerTexel
            )
            try await gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.generateMipmaps(for: texture)
                blit.endEncoding()
            }
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

/// 4x MSAA resolved: a triangle with no edge on the pixel grid, each pixel's coverage worked out at the device's own
/// sample positions; resolved to a texture and to level 1 of a mipmapped one. The rare pixel whose sample lies on an
/// edge (where the fill rule decides) is not checked.
struct MSAAResolveScene: GPUScene {
    let id = "gpu.msaa.resolve"
    let name = "4x MSAA resolve against sample coverage"

    static let size = 64
    /// Vulkan's and Direct3D's standard 4x positions (Apple's GPUs answer the same), for a device that answers none.
    static let standard: [SIMD2<Double>] = [
        SIMD2(0.375, 0.125), SIMD2(0.875, 0.375), SIMD2(0.125, 0.625), SIMD2(0.625, 0.875),
    ]
    static let triangle: [SIMD2<Double>] = [SIMD2(1101, 947), SIMD2(15130, 2611), SIMD2(5274, 15078)].map { $0 / 256 }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        guard gpu.device.supportsTextureSampleCount(4) else {
            result.expect(false, "4x MSAA unsupported")
            return result
        }
        let answered = gpu.device.getDefaultSamplePositions(sampleCount: 4).map {
            SIMD2<Double>(Double($0.x), Double($0.y))
        }
        let usable =
            Set(answered.map { "\($0)" }).count == 4
            && answered.allSatisfy { $0.x >= 0 && $0.x < 1 && $0.y >= 0 && $0.y < 1 }
        result.metrics.set("positions", usable ? "device" : "standard")
        let coverage = Self.coverage(usable ? answered : Self.standard)
        let size = Self.size
        for format in [TexelFormat.rgba8Unorm, .rgba16Float] {
            for resolveLevel in [0, 1] {
                let samples = try gpu.texture(format.metal, size, size, usage: [.renderTarget], samples: 4)
                let resolved = try gpu.texture(
                    format.metal, size << resolveLevel, size << resolveLevel, mipmaps: resolveLevel + 1)
                let pipeline = try gpu.render(fragment: "solid_fragment", format: format.metal, samples: 4)
                try await gpu.run { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = samples
                    pass.colorAttachments[0].resolveTexture = resolved
                    pass.colorAttachments[0].resolveLevel = resolveLevel
                    pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                    pass.colorAttachments[0].storeAction = .multisampleResolve
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(pipeline)
                    var corners = Self.triangle.map {
                        SIMD4<Float>(Float($0.x / Double(size) * 2 - 1), Float(1 - $0.y / Double(size) * 2), 0.5, 1)
                    }
                    var white = SIMD4<Float>(1, 1, 1, 1)
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 3, index: 0)
                    encoder.setFragmentBytes(&white, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
                    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                    encoder.endEncoding()
                }
                let got = try await gpu.raster(resolved, format, level: resolveLevel)
                let want = Raster(width: size, height: size, channels: format.channels, levels: format.levels) {
                    x, y, c in
                    guard let covered = coverage[y * size + x] else { return .nan }
                    return c == 3 ? 1 : Float(covered) / 4
                }
                result.compare(
                    "\(format.rawValue) resolve level \(resolveLevel)", got, want, tolerance: 1.01 * format.step())
            }
        }
        return result
    }

    /// Covered samples per pixel; nil where a sample sits on an edge.
    static func coverage(_ positions: [SIMD2<Double>]) -> [Int?] {
        let t = triangle
        func edge(_ a: SIMD2<Double>, _ b: SIMD2<Double>, _ p: SIMD2<Double>) -> Double {
            (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)
        }
        return (0..<(size * size)).map { index in
            var covered = 0
            for offset in positions {
                let p = SIMD2<Double>(Double(index % size), Double(index / size)) + offset
                let e = [edge(t[0], t[1], p), edge(t[1], t[2], p), edge(t[2], t[0], p)]
                if e.contains(where: { abs($0) < 1e-9 }) { return nil }
                if e.allSatisfy({ $0 > 0 }) || e.allSatisfy({ $0 < 0 }) { covered += 1 }
            }
            return covered
        }
    }
}
