import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Textures over planes of IOSurfaces made by IOSurfaceCreate, read by a kernel (read_float_texture): what the XR and
/// YCbCr surface-form tests share.
public enum SurfaceReads {
    public struct Plane: Sendable {
        let offset, width, height, bytesPerElement, pitch: Int

        public init(at offset: Int, _ width: Int, _ height: Int, bpe: Int, rows pitch: Int) {
            (self.offset, self.width, self.height, self.bytesPerElement, self.pitch) = (
                offset,
                width,
                height,
                bpe,
                pitch
            )
        }
    }

    /// Byte i of every scene's surface
    public static func pattern(_ i: Int) -> UInt8 { UInt8((i * 37 + 11 + (i >> 8) * 59) & 255) }

    /// A surface of `planes` (none: a planeless one of `rows` bytes a row), every byte `fill(i)`
    public static func surface(
        _ fourcc: String, _ width: Int, _ height: Int, planes: [Plane] = [], rows: Int = 0, bpe: Int = 4,
        allocation: Int, fill: (Int) -> UInt8 = pattern
    ) -> IOSurfaceRef? {
        var properties: [CFString: Any] = [
            kIOSurfaceWidth: width, kIOSurfaceHeight: height, kIOSurfaceAllocSize: allocation,
            kIOSurfacePixelFormat: fourcc.utf8.reduce(UInt32(0)) { $0 << 8 | UInt32($1) },
        ]
        if planes.isEmpty {
            properties[kIOSurfaceBytesPerElement] = bpe
            properties[kIOSurfaceBytesPerRow] = rows
        } else {
            properties[kIOSurfacePlaneInfo] = planes.map { p -> [CFString: Any] in
                [
                    kIOSurfacePlaneWidth: p.width, kIOSurfacePlaneHeight: p.height,
                    kIOSurfacePlaneBytesPerElement: p.bytesPerElement, kIOSurfacePlaneBytesPerRow: p.pitch,
                    kIOSurfacePlaneOffset: p.offset, kIOSurfacePlaneSize: p.pitch * p.height,
                ]
            }
        }
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else { return nil }
        IOSurfaceLock(surface, [], nil)
        let base = IOSurfaceGetBaseAddress(surface).assumingMemoryBound(to: UInt8.self)
        for i in 0 ..< IOSurfaceGetAllocSize(surface) { base[i] = fill(i) }
        IOSurfaceUnlock(surface, [], nil)
        return surface
    }

    public static func texture(
        _ gpu: GPU, _ format: UInt, _ width: Int, _ height: Int, over surface: IOSurfaceRef, plane: Int = 0,
        usage: MTLTextureUsage = [.shaderRead]
    ) -> MTLTexture? {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: format)!, width: width, height: height, mipmapped: false
        )
        descriptor.usage = usage
        descriptor.storageMode = .shared
        return gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: plane)
    }

    /// Every texel as read_float_texture reads it, row by row; throws the facade's path where the read failed
    public static func read(_ gpu: GPU, _ texture: MTLTexture) async throws -> [SIMD4<Float>] {
        let count = texture.width * texture.height
        let out = try gpu.buffer(count * 16)
        let pipeline = try gpu.compute("read_float_texture")
        let timing = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: texture.width, height: texture.height, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
            )
            encoder.endEncoding()
        }
        guard !timing.path.hasPrefix("error:") else { throw TestFailure("the read failed: \(timing.facadePath)") }
        return Array(UnsafeBufferPointer(
            start: out.contents().bindMemory(to: SIMD4<Float>.self, capacity: count),
            count: count
        ))
    }

    /// A kernel writing `value` to every texel of `texture` (xr_fill_kernel); the facade's path
    public static func fill(_ gpu: GPU, _ texture: MTLTexture, _ value: SIMD4<Float>) async throws -> String {
        let pipeline = try gpu.compute("xr_fill_kernel")
        var v = value
        return try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBytes(&v, length: 16, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: texture.width, height: texture.height, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
            )
            encoder.endEncoding()
        }.facadePath
    }

    public static func bytes(_ surface: IOSurfaceRef) -> [UInt8] { YcbcrOnePlaneTargets.bytes(surface) }

    public static func word(_ bytes: [UInt8], _ at: Int) -> UInt32 {
        (0 ..< 4).reduce(UInt32(0)) { $0 | UInt32(bytes[at + $1]) << (8 * $1) }
    }

    /// An XR10 word's colour, codes from bit 20 down as (code - 384) / 510, with `alpha`
    public static func xr(_ word: UInt32, alpha: Float) -> SIMD4<Float> {
        let code = { (shift: UInt32) in (Float((word >> shift) & 0x3ff) - 384) / 510 }
        return SIMD4(code(20), code(10), code(0), alpha)
    }

    /// Texels with a channel further than `tolerance` from `want`'s
    public static func off(_ got: [SIMD4<Float>], tolerance: Float = 1e-6, _ want: (Int) -> SIMD4<Float>) -> Int {
        got.indices.filter { i in (0 ..< 4).contains { abs(got[i][$0] - want(i)[$0]) > tolerance } }.count
    }
}
