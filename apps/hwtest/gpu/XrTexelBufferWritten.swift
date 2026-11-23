import Foundation
import HWTestCore
import IOSurface
import Metal

/// gpu.gm20b.xr_texel_buffer_written: a 64-texel RGB10A8_2P_XR10 (550) or sRGB (551) texture buffer, ShaderRead and
/// ShaderWrite, over a 16384x1 'BGRA' surface of 4-byte elements every byte 0xff, written (0.5, 0.25, -0.25, 0.75) by a
/// kernel or by a fragment function at its column, then read back by a kernel
public enum XrTexelBufferWritten {
    public static let texels = 64
    public static let value = SIMD4<Float>(0.5, 0.25, -0.25, 0.75)

    public struct Arm: Sendable {
        public var made = false
        public var status = "-"
        /// Words of the 64 not `word`, bytes past them that changed, and the 64 texels as read after the store
        public var wordsOff = -1
        public var bytesOutside = -1
        public var first: UInt32 = 0
        public var read: [SIMD4<Float>] = []
    }

    public static func observe(_ gpu: GPU, format: UInt, fragment: Bool, word: UInt32) async throws -> Arm {
        let surface = try Self.surface()
        var arm = Arm()
        let descriptor = MTLTextureDescriptor.textureBufferDescriptor(
            with: MTLPixelFormat(rawValue: format)!, width: texels, resourceOptions: [],
            usage: [.shaderRead, .shaderWrite]
        )
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            return arm
        }
        arm.made = true
        arm.status = fragment ? try await drawn(gpu, texture) : try await stored(gpu, texture)
        guard !arm.status.hasPrefix("error:") else { return arm }
        let bytes = SurfaceReads.bytes(surface)
        arm.first = SurfaceReads.word(bytes, 0)
        arm.wordsOff = (0 ..< texels).filter { SurfaceReads.word(bytes, 4 * $0) != word }.count
        arm.bytesOutside = bytes[(4 * texels)...].reduce(0) { $0 + ($1 == 0xff ? 0 : 1) }
        arm.read = try await read(gpu, texture)
        return arm
    }

    static func surface() throws -> IOSurfaceRef {
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: 16384, kIOSurfaceHeight: 1, kIOSurfaceBytesPerElement: 4,
            kIOSurfacePixelFormat: 0x4247_5241,
        ]
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else { throw TestFailure("no 'BGRA' surface") }
        IOSurfaceLock(surface, [], nil)
        IOSurfaceGetBaseAddress(surface).initializeMemory(
            as: UInt8.self, repeating: 0xff, count: IOSurfaceGetAllocSize(surface)
        )
        IOSurfaceUnlock(surface, [], nil)
        return surface
    }

    static func stored(_ gpu: GPU, _ texture: MTLTexture) async throws -> String {
        let pipeline = try gpu.compute("xr_tbuf_written_kernel")
        var v = value
        return try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBytes(&v, length: 16, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: texels, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)
            )
            encoder.endEncoding()
        }.path
    }

    /// A 64x1 RGBA8 target the draw covers whole, its fragments each writing the value at their column
    static func drawn(_ gpu: GPU, _ texture: MTLTexture) async throws -> String {
        let pipeline = try gpu.render(
            vertex: "surface_stores_rect", fragment: "xr_tbuf_written_fragment", format: .rgba8Unorm
        )
        let target = try gpu.texture(.rgba8Unorm, texels, 1, usage: [.renderTarget], storage: .private)
        var rect = SIMD4<Float>(-1, -1, 1, 1), v = value
        return try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .dontCare
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setVertexBytes(&rect, length: 16, index: 0)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.setFragmentBytes(&v, length: 16, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }.path
    }

    static func read(_ gpu: GPU, _ texture: MTLTexture) async throws -> [SIMD4<Float>] {
        let out = try gpu.buffer(texels * 16)
        let pipeline = try gpu.compute("read_texture_buffer")
        let path = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: texels, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)
            )
            encoder.endEncoding()
        }.path
        guard !path.hasPrefix("error:") else { throw TestFailure("the read failed: \(path)") }
        let texelsRead = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: texels)
        return Array(UnsafeBufferPointer(start: texelsRead, count: texels))
    }
}
