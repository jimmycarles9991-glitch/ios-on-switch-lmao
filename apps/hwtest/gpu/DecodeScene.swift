import Foundation
import HWTestCore
import Metal

/// The decode tests' Metal work: textures of the formats the driver decodes on upload (PVRTC, ASTC 4x2/8x4, ASTC HDR:
/// drivers/TegraGM20BMetal/decoded.h), their uploads, and a kernel that read()s chosen texels
/// (shaders/DecodeReads.metal).
public enum DecodeScene {
    /// A texture of `format`, or nil where the device makes none (an identity's refusal is a finding the caller judges)
    public static func texture(
        _ gpu: GPU, _ format: MTLPixelFormat, _ width: Int, _ height: Int, storage: MTLStorageMode = .private,
        levels: Int = 1, layers: Int = 1, usage: MTLTextureUsage = [.shaderRead, .pixelFormatView]
    ) -> MTLTexture? {
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: width, height: height, mipmapped: false
        )
        d.textureType = layers > 1 ? .type2DArray : .type2D
        d.arrayLength = layers
        d.mipmapLevelCount = levels
        d.storageMode = storage
        d.usage = usage
        return gpu.device.makeTexture(descriptor: d)
    }

    /// `texture`'s texels at `at` as read() gives them: of level `level`, or of slice `slice` of an array
    public static func read(
        _ gpu: GPU, _ texture: MTLTexture, _ at: [SIMD2<UInt32>], level: Int = 0, slice: Int = 0
    ) async throws -> [SIMD4<Float>] {
        let array = texture.textureType == .type2DArray
        let state = try gpu.compute(array ? "decode_read_slice_points" : "decode_read_points")
        let points = try gpu.buffer(at)
        let out = try gpu.buffer(at.count * 16)
        try await gpu.run { commands in
            let e = commands.makeComputeCommandEncoder()!
            e.setComputePipelineState(state)
            e.setTexture(texture, index: 0)
            e.setBuffer(points, offset: 0, index: 0)
            e.setBuffer(out, offset: 0, index: 1)
            var which = SIMD2<UInt32>(UInt32(array ? slice : level), UInt32(at.count))
            e.setBytes(&which, length: 8, index: 2)
            e.dispatchThreadgroups(
                MTLSize(width: (at.count + 63) / 64, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)
            )
            e.endEncoding()
        }
        let texels = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: at.count)
        return Array(UnsafeBufferPointer(start: texels, count: at.count))
    }

    /// Every texel of a level of `width` x `height`, row by row
    public static func everyTexel(_ width: Int, _ height: Int) -> [SIMD2<UInt32>] {
        (0 ..< height).flatMap { y in (0 ..< width).map { SIMD2(UInt32($0), UInt32(y)) } }
    }

    /// A blit of `bytes`, placed at `offset` of a shared buffer, into a region of `texture` at the origin
    @discardableResult
    public static func upload(
        _ gpu: GPU, _ bytes: [UInt8], offset: Int = 0, rowBytes: Int, into texture: MTLTexture, width: Int,
        height: Int, level: Int = 0, slice: Int = 0, options: MTLBlitOption = []
    ) async throws -> String {
        let staging = try gpu.buffer(offset + bytes.count)
        bytes.withUnsafeBytes { (staging.contents() + offset).copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
        return try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: staging, sourceOffset: offset, sourceBytesPerRow: rowBytes, sourceBytesPerImage: 0,
                sourceSize: MTLSize(width: width, height: height, depth: 1), to: texture, destinationSlice: slice,
                destinationLevel: level, destinationOrigin: MTLOrigin(), options: options
            )
            blit.endEncoding()
        }.path
    }

    /// An ASTC vector set's blocks in one row of a texture of `format` (its own unless given), uploaded by a blit from
    /// `offset` of a shared buffer: Private, ShaderRead and PixelFormatView; nil where the device makes none
    public static func astcRow(
        _ gpu: GPU, _ set: DecodeVectors.Astc, format: MTLPixelFormat? = nil, offset: Int = 48,
        storage: MTLStorageMode = .private, rotated: Int = 0
    ) async throws -> MTLTexture? {
        guard let texture = texture(gpu, format ?? set.pixelFormat, set.width, set.blockH, storage: storage) else {
            return nil
        }
        try await upload(
            gpu, set.bytes(rotated: rotated), offset: offset, rowBytes: set.blocks.count * 16, into: texture,
            width: set.width, height: set.blockH
        )
        return texture
    }

    /// Reads that differ: bit for bit, a NaN matching any NaN
    public static func differing(_ a: [SIMD4<Float>], _ b: [SIMD4<Float>]) -> Int {
        guard a.count == b.count else { return max(a.count, b.count) }
        return zip(a, b).filter { pair in
            (0 ..< 4).contains { c in
                let x = pair.0[c], y = pair.1[c]
                return !(x.isNaN && y.isNaN) && x.bitPattern != y.bitPattern
            }
        }.count
    }
}
