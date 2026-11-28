import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// R10Unorm_PACKED (588) 32x16 over plane 0 and RG10Unorm_PACKED (589) 16x8 over plane 1 of a 'p420' 32x16 (plane 0
/// rows 64 at 0, plane 1 rows 64 at 1024, every byte seeded), of three levels (gpu.gm20b.packed_levels). Level-0 texel X
/// of row Y, channel c < k (1 for 588, 2 for 589), is the 10-bit code at bit 8 * offset + 8 * 64 * Y + 32 * (n / 3) +
/// 10 * (n % 3), n = k * X + c, over 1023; the other channels 0, alpha 1. An M4 Max makes each with three levels each
/// half the last; its getBytes writes nothing, its replaceRegion and generateMipmaps change no byte of the surface; its
/// reads and samples at any level read level 0 in some runs and displaced bytes in others (a race): counted, not
/// judged.
public enum PackedLevels {
    typealias P = SurfaceReads.Plane
    public static let cases: [(format: UInt, width: Int, height: Int, plane: Int)] = [(588, 32, 16, 0), (589, 16, 8, 1)]
    static let levels = 3, rows = 64

    public struct Reading: Sendable {
        public var made = false
        /// Texels of every level off level 0's texel at the same (x, y), by read() and by a nearest sample
        public var readWrong = -1, sampleWrong = -1
        /// Sizes a kernel sees off W >> L, H >> L; levels it sees off 3
        public var dimsWrong = -1, levelsWrong = -1
        /// Buffer bytes getBytes of each level wrote over its sentinel; surface bytes replaceRegion of levels 0 and 1
        /// and generateMipmaps changed
        public var getBytesWritten = -1, replaceChanged = -1, generateChanged = -1
        /// Level 1's texel (0, 0), red, by a sampler minifying linearly and magnifying nearest (recorded)
        public var minmagCode = -1
    }

    static func surface() -> IOSurfaceRef? {
        let planes = [P(at: 0, 32, 16, bpe: 1, rows: rows), P(at: 1024, 16, 8, bpe: 2, rows: rows)]
        return SurfaceReads.surface("p420", 32, 16, planes: planes, allocation: 16384, fill: PlaneCopies.seeded(0x9e4))
    }

    /// Level-0 texel (x, y) of a `format` texture over the plane at `offset`, from `bytes`
    static func texel(_ bytes: [UInt8], _ format: UInt, offset: Int, _ x: Int, _ y: Int) -> SIMD4<Float> {
        let k = format == 588 ? 1 : 2
        var value = SIMD4<Float>(0, 0, 0, 1)
        for c in 0 ..< k {
            let n = k * x + c, bit = 8 * offset + 8 * rows * y + 32 * (n / 3) + 10 * (n % 3)
            let word = SurfaceReads.word(bytes, bit / 32 * 4)
            value[c] = Float((word >> UInt32(bit % 32)) & 0x3ff) / 1023
        }
        return value
    }

    static func kernel(
        _ gpu: GPU, _ name: String, _ texture: MTLTexture, level: Int, width: Int, height: Int
    ) async throws -> [SIMD4<Float>] {
        let out = try gpu.buffer(width * height * 16), pipeline = try gpu.compute(name)
        var at = UInt32(level)
        let path = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.setBytes(&at, length: 4, index: 1)
            encoder.dispatchThreads(
                MTLSize(width: width, height: height, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
            )
            encoder.endEncoding()
        }.facadePath
        guard !path.hasPrefix("error") else { throw TestFailure("\(name) at level \(level) failed: \(path)") }
        let texels = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: width * height)
        return Array(UnsafeBufferPointer(start: texels, count: width * height))
    }

    public static func observe(
        _ gpu: GPU, _ c: (format: UInt, width: Int, height: Int, plane: Int)
    ) async throws -> Reading {
        var r = Reading()
        guard let surface = surface() else { throw TestFailure("no 'p420' 32x16") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: c.format)!, width: c.width, height: c.height, mipmapped: false
        )
        descriptor.mipmapLevelCount = levels
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: c.plane) else {
            return r
        }
        r.made = true
        let bytes = SurfaceReads.bytes(surface), offset = c.plane * 1024
        let level0 = { (x: Int, y: Int) in texel(bytes, c.format, offset: offset, x, y) }
        let tolerance: Float = 0.5 / 1023
        let near = { (a: SIMD4<Float>, b: SIMD4<Float>) in (0 ..< 4).allSatisfy { abs(a[$0] - b[$0]) <= tolerance } }
        r.readWrong = 0
        r.sampleWrong = 0
        for level in 0 ..< levels {
            let (w, h) = (c.width >> level, c.height >> level)
            let read = try await kernel(gpu, "plevels_read", texture, level: level, width: w, height: h)
            let sampled = try await kernel(gpu, "plevels_sample", texture, level: level, width: w, height: h)
            let half = level == 0 ? 0 : 1 << (level - 1)
            for y in 0 ..< h {
                for x in 0 ..< w {
                    if !near(read[y * w + x], level0(x, y)) { r.readWrong += 1 }
                    let centre = level0((x << level) + half, (y << level) + half)
                    if !near(sampled[y * w + x], centre) { r.sampleWrong += 1 }
                }
            }
        }
        let seen = try gpu.buffer(7 * 4), sizes = try gpu.compute("forms_open_levels")
        _ = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(sizes)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(seen, offset: 0, index: 0)
            let one = MTLSize(width: 1, height: 1, depth: 1)
            encoder.dispatchThreads(one, threadsPerThreadgroup: one)
            encoder.endEncoding()
        }
        let got = Array(UnsafeBufferPointer(start: seen.contents().bindMemory(to: UInt32.self, capacity: 7), count: 7))
        r.levelsWrong = (got[0] == UInt32(levels) ? 0 : 1) + (texture.mipmapLevelCount == levels ? 0 : 1)
        r.dimsWrong = (0 ..< levels).filter { l in
            got[1 + 2 * l] != UInt32(c.width >> l) || got[2 + 2 * l] != UInt32(c.height >> l)
        }.count
        let (w1, h1) = (c.width >> 1, c.height >> 1)
        let minmag = try await kernel(gpu, "plevels_minmag", texture, level: 1, width: w1, height: h1)
        r.minmagCode = Int((minmag[0].x * 1023).rounded())

        r.getBytesWritten = 0
        for level in 0 ..< levels {
            let (w, h) = (c.width >> level, c.height >> level)
            var out = [UInt8](repeating: 0xee, count: 8 * w * h)
            out.withUnsafeMutableBytes {
                let region = MTLRegionMake2D(0, 0, w, h)
                texture.getBytes($0.baseAddress!, bytesPerRow: 8 * w, from: region, mipmapLevel: level)
            }
            r.getBytesWritten += out.filter { $0 != 0xee }.count
        }
        let pattern = (0 ..< 8 * c.width * c.height).map { UInt8(truncatingIfNeeded: $0 * 29 + 7) }
        for level in 0 ..< 2 {
            let (w, h) = (c.width >> level, c.height >> level)
            pattern.withUnsafeBytes {
                texture.replace(
                    region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: level, withBytes: $0.baseAddress!,
                    bytesPerRow: 8 * w
                )
            }
        }
        let replaced = SurfaceReads.bytes(surface)
        r.replaceChanged = zip(replaced, bytes).filter { $0 != $1 }.count
        _ = try await gpu.run(tolerateError: true) { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.generateMipmaps(for: texture)
            blit.endEncoding()
        }
        r.generateChanged = zip(SurfaceReads.bytes(surface), replaced).filter { $0 != $1 }.count
        return r
    }
}
