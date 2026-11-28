import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// What an M4 Max does with views and shared samplers of R10Unorm_PACKED (588) and RG10Unorm_PACKED (589) textures of
/// three levels over gpu.gm20b.packed_levels's 'p420' (gpu.gm20b.packed_levels_open): views of levels 1-2, 2 and 0
/// are made at those levels' sizes and a kernel sees them so; a kernel's one constexpr sampler over such a texture and
/// an RGBA8 one completes and samples the RGBA8 one's texels.
public enum PackedLevelsOpen {
    public static let views: [(first: Int, count: Int)] = [(1, 2), (2, 1), (0, 1)]

    static func texture(
        _ gpu: GPU, _ c: (format: UInt, width: Int, height: Int, plane: Int), over surface: IOSurfaceRef
    ) -> MTLTexture? {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: c.format)!, width: c.width, height: c.height, mipmapped: false
        )
        descriptor.mipmapLevelCount = 3
        descriptor.usage = [.shaderRead, .pixelFormatView]
        descriptor.storageMode = .shared
        return gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: c.plane)
    }

    /// Of the three views of `c`'s texture, those made, and those whose size, level count or kernel-seen sizes are off
    public static func views(
        _ gpu: GPU, _ c: (format: UInt, width: Int, height: Int, plane: Int)
    ) async throws -> (made: Int, wrong: Int) {
        guard let surface = PackedLevels.surface() else { throw TestFailure("no 'p420' 32x16") }
        guard let texture = texture(gpu, c, over: surface) else { return (0, 0) }
        var made = 0, wrong = 0
        let sizes = try gpu.compute("forms_open_levels")
        for v in views {
            guard
                let view = texture.makeTextureView(
                    pixelFormat: texture.pixelFormat, textureType: .type2D, levels: v.first ..< (v.first + v.count),
                    slices: 0 ..< 1
                )
            else { continue }
            made += 1
            let (w, h) = (c.width >> v.first, c.height >> v.first)
            let seen = try gpu.buffer(7 * 4)
            let path = try await gpu.run(tolerateError: true) { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(sizes)
                encoder.setTexture(view, index: 0)
                encoder.setBuffer(seen, offset: 0, index: 0)
                let one = MTLSize(width: 1, height: 1, depth: 1)
                encoder.dispatchThreads(one, threadsPerThreadgroup: one)
                encoder.endEncoding()
            }.facadePath
            let got = seen.contents().bindMemory(to: UInt32.self, capacity: 7)
            let kernelSees = got[0] == UInt32(v.count) && got[1] == UInt32(w) && got[2] == UInt32(h)
            if view.width != w || view.height != h || view.mipmapLevelCount != v.count || path.hasPrefix("error")
                || !kernelSees
            {
                wrong += 1
            }
        }
        return (made, wrong)
    }

    /// A kernel sampling `c`'s texture and an RGBA8 8x8 with one constexpr sampler: its facade path and the RGBA8
    /// texels off what it holds
    public static func sharedSampler(
        _ gpu: GPU, _ c: (format: UInt, width: Int, height: Int, plane: Int)
    ) async throws -> (path: String, off: Int) {
        guard let surface = PackedLevels.surface() else { throw TestFailure("no 'p420' 32x16") }
        guard let texture = texture(gpu, c, over: surface) else { return ("no texture", -1) }
        let plain = try gpu.texture(.rgba8Unorm, 8, 8, usage: [.shaderRead], storage: .shared)
        let texels = (0 ..< 256).map { UInt8(truncatingIfNeeded: $0 * 7 + 1) }
        try await gpu.upload(texels, to: plain, bytesPerRow: 32)
        let out = try gpu.buffer(64 * 16), packedOut = try gpu.buffer(64 * 16)
        let pipeline = try gpu.compute("plevels_shared")
        let path = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setTexture(plain, index: 1)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.setBuffer(packedOut, offset: 0, index: 1)
            encoder.dispatchThreads(
                MTLSize(width: 8, height: 8, depth: 1), threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
            )
            encoder.endEncoding()
        }.facadePath
        guard !path.hasPrefix("error") else { return (path, -1) }
        let got = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: 64)
        let off = (0 ..< 64).filter { i in
            (0 ..< 4).contains { abs(got[i][$0] - Float(texels[4 * i + $0]) / 255) > 0.5 / 255 }
        }
        return (path, off.count)
    }
}
