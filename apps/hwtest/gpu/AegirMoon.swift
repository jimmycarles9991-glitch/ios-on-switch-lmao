import Foundation
import HWTestCore
import IOSurface
import Metal

/// gpu.gm20b.aegir_moon's frame: the pass structure of Weather's moon card as NanoUniverse's Aegir renderer draws its first
/// frame on a fresh queue (shaders/AegirMoon.metal: a disc stands in for luna's ray-traced sphere). A 172x172
/// BGR10_XR_sRGB drawable (a CAMetalLayer's, as Weather's, or one over a 'w30r' IOSurface) and a mask over an 'A008'
/// IOSurface through an R8Unorm texture, in one command buffer of three passes:
///  1. scene: the drawable cleared to 0 and the mask to 1, a disc drawn source-over into both;
///  2. bloom: the drawable's threshold into an 86x86 R8Unorm texture of three levels, then generateMipmaps;
///  3. post: both loaded, nothing added to the colour (one/one), bloom level 2 taken off the mask (one/one reverse
///     subtract).
/// Outside the disc Metal leaves the mask at 255, the bloom at 0 away from it and the drawable at XR 0.0 (code 384).
public enum AegirMoon {
    public static let side = 172, bloomSide = 86, xrZero: UInt32 = 384
    /// The disc's grey 0.8 as an M4's GPU stores it in BGR10_XR_sRGB (host_gpu_tests/AegirMoonTests.swift)
    public static let discCode: UInt32 = 846

    public struct Reading: Sendable {
        public var mask: [UInt8] = [] // side x side, rows packed, read under the surface's lock
        public var bloom2: [UInt8] = [] // the bloom chain's level 2, rows packed
        public var drawable: [UInt32] = [] // side x side words
        public var path = ""
        public var drawableFacts = "", maskFacts = "" // the driver's layout of each (switchMetalProbeFacts:)
    }

    static func surface(_ fourcc: String, bytesPerElement: Int) throws -> IOSurfaceRef {
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: side, kIOSurfaceHeight: side, kIOSurfaceBytesPerElement: bytesPerElement,
            kIOSurfacePixelFormat: fourcc.utf8.reduce(UInt32(0)) { $0 << 8 | UInt32($1) },
        ]
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else {
            throw TestFailure("IOSurfaceCreate('\(fourcc)') returned nil")
        }
        return surface
    }

    static func texture(_ gpu: GPU, _ surface: IOSurfaceRef, _ format: MTLPixelFormat) throws -> MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format,
            width: side,
            height: side,
            mipmapped: false
        )
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: d, iosurface: surface, plane: 0) else {
            throw TestFailure("no \(format.rawValue) texture over the surface")
        }
        return texture
    }

    static func rows<T>(_ surface: IOSurfaceRef, _: T.Type) -> [T] {
        IOSurfaceLock(surface, .readOnly, nil)
        defer { IOSurfaceUnlock(surface, .readOnly, nil) }
        let base = IOSurfaceGetBaseAddress(surface), pitch = IOSurfaceGetBytesPerRow(surface)
        return (0 ..< side).flatMap { y in
            Array(UnsafeBufferPointer(start: (base + y * pitch).assumingMemoryBound(to: T.self), count: side))
        }
    }

    static func pipeline(
        _ gpu: GPU, _ fragment: String, _ formats: [MTLPixelFormat],
        blend: (MTLRenderPipelineColorAttachmentDescriptor, Int) -> Void = { _, _ in }
    ) throws -> MTLRenderPipelineState {
        let d = MTLRenderPipelineDescriptor()
        d.vertexFunction = gpu.library.makeFunction(name: "aegir_moon_vertex")
        d.fragmentFunction = gpu.library.makeFunction(name: fragment)
        for (j, format) in formats.enumerated() {
            d.colorAttachments[j].pixelFormat = format
            blend(d.colorAttachments[j], j)
        }
        return try gpu.device.makeRenderPipelineState(descriptor: d)
    }

    static func blend(
        _ a: MTLRenderPipelineColorAttachmentDescriptor, _ op: MTLBlendOperation, _ src: MTLBlendFactor,
        _ dst: MTLBlendFactor
    ) {
        a.isBlendingEnabled = true
        a.rgbBlendOperation = op
        a.alphaBlendOperation = op
        a.sourceRGBBlendFactor = src
        a.sourceAlphaBlendFactor = src
        a.destinationRGBBlendFactor = dst
        a.destinationAlphaBlendFactor = dst
    }

    static func targets(_ colour: MTLTexture, _ mask: MTLTexture, load: Bool) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        for (j, texture) in [colour, mask].enumerated() {
            pass.colorAttachments[j].texture = texture
            pass.colorAttachments[j].loadAction = load ? .load : .clear
            pass.colorAttachments[j].storeAction = .store
        }
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        pass.colorAttachments[1].clearColor = MTLClearColor(red: 1, green: 1, blue: 1, alpha: 1)
        return pass
    }

    /// The frame into `given` (a CAMetalLayer drawable's texture, side x side), or into a texture over a 'w30r' surface
    public static func run(
        _ gpu: GPU, drawable given: MTLTexture? = nil, facts: (MTLTexture) -> String = { _ in "" }
    ) async throws -> Reading {
        let drawableSurface = given == nil ? try surface("w30r", bytesPerElement: 4) : nil
        let maskSurface = try surface("A008", bytesPerElement: 1)
        let drawable = try given ?? texture(gpu, drawableSurface!, .bgr10_xr_srgb)
        let mask = try texture(gpu, maskSurface, .r8Unorm)
        let bloom = try gpu.texture(.r8Unorm, bloomSide, bloomSide, mipmaps: 3)
        let formats: [MTLPixelFormat] = [.bgr10_xr_srgb, .r8Unorm]
        let scene = try pipeline(gpu, "aegir_moon_scene", formats) { a, _ in
            blend(a, .add, .sourceAlpha, .oneMinusSourceAlpha)
        }
        let threshold = try pipeline(gpu, "aegir_moon_threshold", [.r8Unorm])
        let post = try pipeline(gpu, "aegir_moon_post", formats) { a, j in
            blend(a, j == 0 ? .add : .reverseSubtract, .one, .one)
        }
        let ran = try await gpu.run { commands in
            let first = commands.makeRenderCommandEncoder(descriptor: targets(drawable, mask, load: false))!
            first.setRenderPipelineState(scene)
            first.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            first.endEncoding()
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = bloom
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let second = commands.makeRenderCommandEncoder(descriptor: pass)!
            second.setRenderPipelineState(threshold)
            second.setFragmentTexture(drawable, index: 0)
            second.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            second.endEncoding()
            let mips = commands.makeBlitCommandEncoder()!
            mips.generateMipmaps(for: bloom)
            mips.endEncoding()
            let third = commands.makeRenderCommandEncoder(descriptor: targets(drawable, mask, load: true))!
            third.setRenderPipelineState(post)
            third.setFragmentTexture(bloom, index: 0)
            third.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            third.endEncoding()
        }
        var reading = Reading(path: ran.path)
        reading.mask = rows(maskSurface, UInt8.self)
        if let surface = drawableSurface ?? drawable.iosurface {
            reading.drawable = rows(surface, UInt32.self)
        } else {
            let bytes = try await gpu.read(drawable, bytesPerPixel: 4)
            reading.drawable = stride(from: 0, to: bytes.count, by: 4).map { at in
                (0 ..< 4).reduce(UInt32(0)) { $0 | UInt32(bytes[at + $1]) << (8 * $1) }
            }
        }
        reading.drawableFacts = facts(drawable)
        reading.maskFacts = facts(mask)
        reading.bloom2 = try await gpu.read(bloom, bytesPerPixel: 1, level: 2)
        return reading
    }

    /// A texel's distance from the target's centre, in units of its side
    static func distance(_ x: Int, _ y: Int, side: Int) -> Double {
        let dx = (Double(x) + 0.5) / Double(side) - 0.5, dy = (Double(y) + 0.5) / Double(side) - 0.5
        return (dx * dx + dy * dy).squareRoot()
    }

    static func codes(_ word: UInt32) -> [UInt32] { [word & 0x3ff, word >> 10 & 0x3ff, word >> 20 & 0x3ff] }

    /// Texels unlike Metal's, by check: mask_outside (not 255 past 0.5 of the side from the centre), mask_inside (above 8
    /// within 0.25), bloom_outside (not 0 past 0.6 on level 2), drawable_outside (a channel not XR 0.0 past 0.45),
    /// drawable_inside (a channel not discCode within 0.3)
    public static func misses(_ r: Reading) -> [(String, Int)] {
        let mask = side, bloom = bloomSide >> 2
        func count<T>(_ values: [T], _ width: Int, where test: (T, Double) -> Bool) -> Int {
            values.indices.filter { test(values[$0], distance($0 % width, $0 / width, side: width)) }.count
        }
        return [
            ("mask_outside", count(r.mask, mask) { $1 > 0.5 && $0 != 255 }),
            ("mask_inside", count(r.mask, mask) { $1 < 0.25 && $0 > 8 }),
            ("bloom_outside", count(r.bloom2, bloom) { $1 > 0.6 && $0 != 0 }),
            ("drawable_outside", count(r.drawable, mask) { $1 > 0.45 && codes($0) != [xrZero, xrZero, xrZero] }),
            ("drawable_inside", count(r.drawable, mask) { $1 < 0.3 && codes($0) != [discCode, discCode, discCode] }),
        ]
    }

    /// The texels at the centre and at a corner of each target, as the line names them
    public static func samples(_ r: Reading) -> [(String, String)] {
        let centre = side / 2 * side + side / 2, bloom = bloomSide >> 2
        return [
            ("mask_centre", "\(r.mask[centre])"), ("mask_corner", "\(r.mask[0])"),
            ("bloom2_centre", "\(r.bloom2[bloom / 2 * bloom + bloom / 2])"), ("bloom2_corner", "\(r.bloom2[0])"),
            ("drawable_centre", codes(r.drawable[centre]).map(String.init).joined(separator: "/")),
            ("drawable_corner", codes(r.drawable[0]).map(String.init).joined(separator: "/")),
        ]
    }
}
