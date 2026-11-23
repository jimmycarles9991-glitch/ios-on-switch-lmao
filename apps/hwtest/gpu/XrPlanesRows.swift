import Foundation
import HWTestCore
import IOSurface
import Metal

/// A 550 (RGB10A8_2P_XR10) texture of 64x64 over plane 0 of a 'b3a8' whose rows are off 32 bytes: plane 0 words, rows
/// 272 bytes apart at 0, plane 1 alpha bytes, rows 80 apart at 17408, 22528 bytes (gpu.gm20b.xr_planes_rows_off32). Word
/// (x, y) holds codes (x + 7y) & 0x3ff, 300 + y, 500 + x from bit 20 down; alpha byte (3x + y) & 255.
public enum XrPlanesRows {
    static let size = 64, wordPitch = 272, alphaAt = 17408, alphaPitch = 80, allocation = 22528

    public struct Reading: Sendable {
        public var made = false
        /// read_texel_fragment into RGBA16Float: texels with a channel more than one half step off the words' and
        /// alpha bytes' values, after the fill and after a blit of new words
        public var filledOff = 0, blittedOff = 0
        /// Of those, texels not the nearest halves
        public var filledInexact = 0, blittedInexact = 0
        /// xr_rewrite_alpha: plane 0 words not the blitted ones, plane 1 bytes not 255
        public var wordsOff = 0, alphaOff = 0
        public var first = "-"
        public var paths: [String] = []
    }

    public static func word(_ x: Int, _ y: Int) -> UInt32 {
        UInt32((x + 7 * y) & 0x3ff) << 20 | UInt32(300 + y) << 10 | UInt32(500 + x)
    }

    public static func blitWord(_ x: Int, _ y: Int) -> UInt32 { 0x1801_ffff ^ UInt32(x << 4 | y) }

    static func alpha(_ x: Int, _ y: Int) -> UInt8 { UInt8((3 * x + y) & 255) }

    /// A texel as the draw writes it: each 10-bit code from bit 20 down as (code - 384) / 510 into red, green, blue
    static func texel(_ word: UInt32, _ alpha: UInt8) -> SIMD4<Float16> {
        let code = { (shift: UInt32) in Float16((Float((word >> shift) & 0x3ff) - 384) / 510) }
        return SIMD4(code(20), code(10), code(0), Float16(Float(alpha) / 255))
    }

    static func surface() -> IOSurfaceRef? {
        let planes: [[CFString: Any]] = [
            [
                kIOSurfacePlaneWidth: size, kIOSurfacePlaneHeight: size, kIOSurfacePlaneBytesPerElement: 4,
                kIOSurfacePlaneBytesPerRow: wordPitch, kIOSurfacePlaneOffset: 0, kIOSurfacePlaneSize: wordPitch * size,
            ],
            [
                kIOSurfacePlaneWidth: size, kIOSurfacePlaneHeight: size, kIOSurfacePlaneBytesPerElement: 1,
                kIOSurfacePlaneBytesPerRow: alphaPitch, kIOSurfacePlaneOffset: alphaAt,
                kIOSurfacePlaneSize: alphaPitch * size,
            ],
        ]
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: size, kIOSurfaceHeight: size, kIOSurfacePixelFormat: OSType(0x6233_6138),
            kIOSurfaceAllocSize: allocation, kIOSurfacePlaneInfo: planes,
        ]
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else { return nil }
        IOSurfaceLock(surface, [], nil)
        let base = IOSurfaceGetBaseAddress(surface)
        for y in 0 ..< size {
            for x in 0 ..< size {
                base.storeBytes(of: word(x, y), toByteOffset: y * wordPitch + 4 * x, as: UInt32.self)
                base.storeBytes(of: alpha(x, y), toByteOffset: alphaAt + y * alphaPitch + x, as: UInt8.self)
            }
        }
        IOSurfaceUnlock(surface, [], nil)
        return surface
    }

    /// Texels more than one half step off, and texels not the nearest halves; -1 where the draw failed
    static func drawn(_ gpu: GPU, _ texture: MTLTexture, _ words: (Int, Int) -> UInt32, _ reading: inout Reading)
        async throws -> (off: Int, inexact: Int)
    {
        let target = try gpu.texture(.rgba16Float, size, size)
        let state = try gpu.render(fragment: "read_texel_fragment", format: .rgba16Float)
        let timing = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            var corners = GPU.fullScreen
            encoder.setRenderPipelineState(state)
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        reading.paths.append(timing.facadePath)
        guard !timing.path.hasPrefix("error:") else { return (-1, -1) }
        let bytes = try await gpu.read(target, bytesPerPixel: 8)
        var counts = (off: 0, inexact: 0)
        for y in 0 ..< size {
            for x in 0 ..< size {
                let got = bytes.loaded(as: SIMD4<Float16>.self, at: y * size + x), want = texel(
                    words(x, y),
                    alpha(x, y)
                )
                guard got != want else { continue }
                counts.inexact += 1
                let steps = (0 ..< 4)
                    .map {
                        abs(Int(Int16(bitPattern: got[$0].bitPattern)) - Int(Int16(bitPattern: want[$0].bitPattern))) }
                guard steps.contains(where: { $0 > 1 }) else { continue }
                counts.off += 1
                if reading.first == "-" { reading.first = "\(x),\(y):\(got)_not_\(want)" }
            }
        }
        return counts
    }

    public static func observe(_ gpu: GPU) async throws -> Reading {
        var reading = Reading()
        guard let surface = surface() else { throw TestFailure("no 'b3a8' surface") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: 550)!, width: size, height: size, mipmapped: false
        )
        descriptor.usage = [.shaderRead, .shaderWrite]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0)
        else { return reading }
        reading.made = true
        (reading.filledOff, reading.filledInexact) = try await drawn(gpu, texture, word, &reading)
        let words = try gpu.buffer((0 ..< (size * size)).map { blitWord($0 % size, $0 / size) })
        let blit = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeBlitCommandEncoder()!
            encoder.copy(
                from: words, sourceOffset: 0, sourceBytesPerRow: size * 4, sourceBytesPerImage: size * size * 4,
                sourceSize: MTLSize(width: size, height: size, depth: 1), to: texture, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin()
            )
            encoder.endEncoding()
        }
        reading.paths.append(blit.facadePath)
        (reading.blittedOff, reading.blittedInexact) = try await drawn(gpu, texture, blitWord, &reading)
        let rewrite = try gpu.compute("xr_rewrite_alpha")
        let kernel = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(rewrite)
            encoder.setTexture(texture, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: size, height: size, depth: 1), threadsPerThreadgroup: MTLSize(
                    width: 8,
                    height: 8,
                    depth: 1
                )
            )
            encoder.endEncoding()
        }
        reading.paths.append(kernel.facadePath)
        IOSurfaceLock(surface, .readOnly, nil)
        let base = IOSurfaceGetBaseAddress(surface)
        for y in 0 ..< size {
            for x in 0 ..< size {
                if base
                    .load(fromByteOffset: y * wordPitch + 4 * x, as: UInt32.self) !=
                    blitWord(x, y) { reading.wordsOff += 1 }
                if base
                    .load(fromByteOffset: alphaAt + y * alphaPitch + x, as: UInt8.self) != 255 { reading.alphaOff += 1 }
            }
        }
        IOSurfaceUnlock(surface, .readOnly, nil)
        return reading
    }
}
