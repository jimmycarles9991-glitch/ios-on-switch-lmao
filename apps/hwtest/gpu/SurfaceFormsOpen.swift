import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Textures over surfaces that an M4 Max makes, each through a form of its own on the driver: a texel buffer over a
/// surface the kernel laid out block-linear, R10Unorm_PACKED and RG10Unorm_PACKED of several levels, and a turned
/// RGB10A8_2P_XR10 over planes of rows under 16 bytes.
public enum SurfaceFormsOpen {
    public struct Reading: Sendable {
        public var made = false
        public var path = "-"
        /// Texels or bytes off Apple's
        public var off = -1
    }

    /// gpu.gm20b.xr_texel_buffer_tiled: a 'w30r' 64x64 of rows 256 drawn as BGR10_XR (554) first (on the device the
    /// kernel then lays it out block-linear), then a 128-texel 550 texel buffer over it (Metal asserts on one whose texels
    /// pass the rows of 256): element i reads what the 554 reads at (i % 64, i / 64), alpha 0 (Metal reads such a
    /// buffer's bytes in order)
    public static func texelBufferTiled(_ gpu: GPU) async throws -> Reading {
        let texelCount = 128
        guard let surface = SurfaceReads.surface("w30r", 64, 64, rows: 256, allocation: 16384, fill: { _ in 0 }),
              let target = SurfaceReads.texture(gpu, 554, 64, 64, over: surface, usage: [.renderTarget, .shaderRead])
        else { throw TestFailure("no 554 over a 'w30r'") }
        let clear = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        let drawn = try await RowsUnder16.pass(gpu, target, clear: clear, fragment: "forms_open_pixel_colours")
        guard !drawn.hasPrefix("error") else { throw TestFailure("the 554 pass failed: \(drawn)") }
        let image = try await SurfaceReads.read(gpu, target)
        let descriptor = MTLTextureDescriptor.textureBufferDescriptor(
            with: MTLPixelFormat(rawValue: 550)!, width: texelCount, resourceOptions: [], usage: [.shaderRead]
        )
        guard let texels = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            return Reading()
        }
        let out = try gpu.buffer(texelCount * 16)
        let pipeline = try gpu.compute("read_texture_buffer")
        let path = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texels, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: texelCount, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)
            )
            encoder.endEncoding()
        }.facadePath
        let read = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: texelCount)
        let got = Array(UnsafeBufferPointer(start: read, count: texelCount))
        let off = SurfaceReads.off(got, tolerance: 0.5 / 510) { SIMD4(image[$0].x, image[$0].y, image[$0].z, 0) }
        return Reading(made: true, path: path, off: off)
    }

    /// gpu.gm20b.packed_mips: R10Unorm_PACKED (588) 32x16 over plane 0, or RG10Unorm_PACKED (589) 16x8 over plane 1, of a
    /// CoreVideo 'p420' 32x16 with `levels` levels: made, and a kernel sees that many levels each half the last
    /// (Apple's reads of such a texture at any lod are level 0's, but which tile of it varies: not judged)
    public static func packedMips(_ gpu: GPU, format: UInt, levels: Int) async throws -> Reading {
        var owner: CVPixelBuffer?
        let attributes =
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        guard CVPixelBufferCreate(nil, 32, 16, 0x7034_3230, attributes, &owner) == kCVReturnSuccess, let buffer = owner,
              let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue()
        else { throw TestFailure("no 'p420' pixel buffer") }
        let (width, height, plane) = format == 588 ? (32, 16, 0) : (16, 8, 1)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: format)!, width: width, height: height, mipmapped: false
        )
        descriptor.mipmapLevelCount = levels
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: plane) else {
            return Reading()
        }
        let out = try gpu.buffer(7 * 4)
        let pipeline = try gpu.compute("forms_open_levels")
        let path = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            let one = MTLSize(width: 1, height: 1, depth: 1)
            encoder.dispatchThreads(one, threadsPerThreadgroup: one)
            encoder.endEncoding()
        }.facadePath
        let seen = Array(UnsafeBufferPointer(start: out.contents().bindMemory(to: UInt32.self, capacity: 7), count: 7))
        var want: [UInt32] = [UInt32(levels)]
        for level in 0 ..< 3 {
            want += level < levels ? [UInt32(width >> level), UInt32(height >> level)] : [0, 0]
        }
        let off = zip(seen, want).filter { $0 != $1 }.count + (texture.mipmapLevelCount == levels ? 0 : 1)
        withExtendedLifetime(buffer) {}
        return Reading(made: true, path: path, off: off)
    }

    public enum TurnedPass: String, Sendable, CaseIterable { case clear, draw, load }

    /// gpu.gm20b.xr_turned_rows_under_16: a 550 turned by `rotation` (frame 8x2 at 1 and 3, 2x8 at 2) over
    /// gpu.gm20b.xr_rows_under_16's 'b3a8' 2x8 of rows 8 and 2 in 8 MiB + 16 KiB, over 0xff: a clear of (0.5, 0.25,
    /// -0.25, 0.75), or a whole-frame coded draw, stores frame pixel (fx, fy)'s word and alpha at the plane texel the
    /// rotation turns it to, 16 bytes a row apart; a loaded pass whose draw discards every fragment stores each plane
    /// texel's word and alpha read a MiB a row apart at the same texel 16 apart; where word rows 4 to 7 meet alpha rows 0
    /// to 3 the words land last at rotation 1 and the alpha at rotations 2 and 3 (an M4 Max, three runs)
    public static func turnedRowsUnder16(_ gpu: GPU, rotation: Int, pass: TurnedPass) async throws -> Reading {
        let alphaLast = rotation != 1, mebibyte = RowsUnder16.mebibyte
        guard let surface = RowsUnder16.surface("b3a8", 2, 8, planes: XrRowsUnder16.planes, fill: 0xff) else {
            throw TestFailure("no 'b3a8' 2x8 of rows 8 and 2")
        }
        var read: [Int: UInt8] = [:]
        if pass == .load {
            for y in 0 ..< 8 {
                for b in 0 ..< 8 { read[y * mebibyte + b] = UInt8(1 + 7 * y + b) }
                for x in 0 ..< 2 { read[64 + y * mebibyte + x] = UInt8(truncatingIfNeeded: 9 + 16 * y + x) }
            }
            RowsUnder16.edit(surface) { base in for (at, byte) in read { base[at] = byte } }
        }
        let (fw, fh) = rotation == 2 ? (2, 8) : (8, 2)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: 550)!, width: fw, height: fh, mipmapped: false
        )
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard descriptor.responds(to: NSSelectorFromString("setRotation:")) else {
            throw TestFailure("MTLTextureDescriptor answers no setRotation:")
        }
        descriptor.setValue(NSNumber(value: rotation), forKey: "rotation")
        descriptor.setValue(NSNumber(value: true), forKey: "framebufferOnly")
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            return Reading()
        }
        let fragment = ["draw": "rows16_xr_codes", "load": "rows16_discard"][pass.rawValue]
        let path = try await RowsUnder16.pass(
            gpu, texture, clear: XrRowsUnder16.clear, fragment: fragment, load: pass == .load
        )
        var want = [UInt8](repeating: 0xff, count: 16384)
        for (at, byte) in read where at < 16384 { want[at] = byte }
        let texels = (0 ..< fh).flatMap { fy in (0 ..< fw).map { fx in plane(rotation, fx, fy) } }
        let frame = Dictionary(uniqueKeysWithValues: (0 ..< fh).flatMap { fy in
            (0 ..< fw).map { fx in ("\(plane(rotation, fx, fy))", (fx, fy)) }
        })
        let value = { (p: (x: Int, y: Int)) -> (word: UInt32, alpha: UInt8) in
            switch pass {
            case .clear: return (XrRowsUnder16.clearWord, XrRowsUnder16.clearAlpha)
            case .draw:
                let f = frame["\(p)"]!
                return XrRowsUnder16.coded(f.0, f.1)
            case .load:
                let at = p.y * mebibyte + 4 * p.x
                let word = (0 ..< 4).reduce(UInt32(0)) { $0 | UInt32(read[at + $1] ?? 0xff) << (8 * $1) }
                return (word, read[64 + p.y * mebibyte + p.x] ?? 0xff)
            }
        }
        let alpha = { for p in texels { want[64 + 16 * p.y + p.x] = value(p).alpha } }
        if !alphaLast { alpha() }
        for p in texels { RowsUnder16.put(value(p).word, &want, at: 16 * p.y + 4 * p.x) }
        if alphaLast { alpha() }
        let off = RowsUnder16.off(surface, want, fill: 0xff, except: read.filter { $0.key >= 16384 })
        return Reading(made: true, path: path, off: off)
    }

    /// The plane texel of a 2x8 plane that frame pixel (fx, fy) of a texture turned by `rotation` lands on
    static func plane(_ rotation: Int, _ fx: Int, _ fy: Int) -> (x: Int, y: Int) {
        switch rotation {
        case 1: return (1 - fy, fx)
        case 2: return (1 - fx, 7 - fy)
        default: return (fy, 7 - fx)
        }
    }
}
