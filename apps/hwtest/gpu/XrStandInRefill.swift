import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A 128-texel RGB10A8_2P_XR10 (550) texel buffer over a 'w30r' 64x64 of rows 256 that a BGR10_XR (554) target drew
/// into first (on the device the kernel then lays it out block-linear), read by a kernel after another kernel wrote
/// (0.5, 0.25, -0.25, 0.75) through the 554 (gpu.gm20b.xr_stand_in_refill): in the same compute encoder after a texture
/// memory barrier, or in an encoder of its own. An M4 Max reads the written colour with alpha 0 either way.
public enum XrStandInRefill {
    public static let texels = 128

    public struct Reading: Sendable {
        public var made = false
        public var path = "-"
        /// Elements read off the written colour
        public var off = -1
    }

    public static func observe(_ gpu: GPU, sameEncoder: Bool) async throws -> Reading {
        guard let surface = SurfaceReads.surface("w30r", 64, 64, rows: 256, allocation: 16384, fill: { _ in 0 }),
              let target = SurfaceReads.texture(
                  gpu, 554, 64, 64, over: surface, usage: [.renderTarget, .shaderRead, .shaderWrite]
              )
        else { throw TestFailure("no 554 over a 'w30r' 64x64") }
        let drawn = try await RowsUnder16.pass(
            gpu, target, clear: MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1), fragment: "turned_order_colours"
        )
        guard !drawn.hasPrefix("error") else { throw TestFailure("the 554 pass failed: \(drawn)") }
        let descriptor = MTLTextureDescriptor.textureBufferDescriptor(
            with: MTLPixelFormat(rawValue: 550)!, width: texels, resourceOptions: [], usage: [.shaderRead]
        )
        guard let buffer = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            return Reading()
        }
        let fill = try gpu.compute("xr_fill_kernel"), read = try gpu.compute("read_texture_buffer")
        let out = try gpu.buffer(texels * 16)
        var value = SIMD4<Float>(0.5, 0.25, -0.25, 0.75)
        let path = try await gpu.run(tolerateError: true) { commands in
            var encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(fill)
            encoder.setTexture(target, index: 0)
            encoder.setBytes(&value, length: 16, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: 64, height: 64, depth: 1), threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
            )
            if sameEncoder {
                encoder.memoryBarrier(scope: .textures)
            } else {
                encoder.endEncoding()
                encoder = commands.makeComputeCommandEncoder()!
            }
            encoder.setComputePipelineState(read)
            encoder.setTexture(buffer, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            let line = MTLSize(width: texels, height: 1, depth: 1)
            encoder.dispatchThreads(line, threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
            encoder.endEncoding()
        }.facadePath
        let read4 = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: texels)
        let got = Array(UnsafeBufferPointer(start: read4, count: texels))
        let off = SurfaceReads.off(got, tolerance: 0.5 / 510) { _ in SurfaceReads.xr(0x27f8_0100, alpha: 0) }
        return Reading(made: true, path: path, off: off)
    }
}
