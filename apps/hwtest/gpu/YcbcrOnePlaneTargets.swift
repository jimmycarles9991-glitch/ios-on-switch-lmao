import Foundation
import HWTestCore
import IOSurface
import Metal

/// One-plane YCbCr render targets over a one-plane surface made by IOSurfaceCreate (every byte i * 31 + 7 first), each
/// one command buffer: a Clear (0.5, 0.25, 0.75, 1) and Store pass, or (drawn) a DontCare pass drawing red
/// (3y + x) & 255, green (x + 7y) & 255, blue (4x + y) & 255 (codes / 255). Each case's bytes are the whole allocation as
/// an M4 Max leaves it (gpu.gm20b.ycbcr_onep_targets).
public enum YcbcrOnePlaneTargets {
    public struct Case: Sendable {
        public let key: String
        let format: UInt
        let width, height: Int
        let usage: MTLTextureUsage
        let fourcc: OSType
        let surfaceWidth, bytesPerElement, pitch: Int
        var drawn = false
        /// The allocation Apple leaves, from the one before the pass
        public let expected: @Sendable ([UInt8]) -> [UInt8]
    }

    static let surfaceHeight = 16
    static let read: MTLTextureUsage = [.shaderRead], target: MTLTextureUsage = [.shaderRead, .renderTarget]
    static let fourcc2vuy = OSType(0x3276_7579), fourccYuvs = OSType(0x7975_7673), fourccV410 = OSType(0x7634_3130)

    public static let cases: [Case] = [
        Case(
            key: "pairs_clear", format: 501, width: 64, height: 16, usage: target, fourcc: fourcc2vuy, surfaceWidth: 64,
            bytesPerElement: 2, pitch: 128, expected: { rows($0, pitch: 128, [0x64, 0xaf, 0x64, 0x93], count: 128) }
        ),
        Case(
            key: "pairs_rev_clear", format: 562, width: 64, height: 16, usage: target, fourcc: fourccYuvs,
            surfaceWidth: 64, bytesPerElement: 2, pitch: 128,
            expected: { rows($0, pitch: 128, [0xaf, 0x64, 0x93, 0x64], count: 128) }
        ),
        Case(
            key: "pairs_odd", format: 501, width: 33, height: 16, usage: read, fourcc: fourcc2vuy, surfaceWidth: 64,
            bytesPerElement: 2, pitch: 128,
            expected: { before in
                var out = rows(before, pitch: 128, [0x64, 0xaf, 0x64, 0x93], count: 64)
                for y in 0 ..< surfaceHeight { (out[y * 128 + 64], out[y * 128 + 65], out[y * 128 + 67]) = (
                    0x64,
                    0xaf,
                    0x93
                ) }
                return out
            }
        ),
        Case(
            key: "pairs_odd_drawn", format: 501, width: 33, height: 16, usage: read, fourcc: fourcc2vuy,
            surfaceWidth: 64, bytesPerElement: 2, pitch: 128, drawn: true, expected: { oddDrawn($0) }
        ),
        Case(
            key: "words_clear", format: 504, width: 33, height: 16, usage: read, fourcc: fourccV410, surfaceWidth: 64,
            bytesPerElement: 4, pitch: 256,
            expected: { rows($0, pitch: 256, [0x8e, 0xe9, 0xca, 0x24], count: 33 * 4) }
        ),
    ]

    /// `before` with the first `count` bytes of every row `pattern` repeated
    static func rows(_ before: [UInt8], pitch: Int, _ pattern: [UInt8], count: Int) -> [UInt8] {
        var out = before
        for y in 0 ..< surfaceHeight { for i in 0 ..< count { out[y * pitch + i] = pattern[i % pattern.count] } }
        return out
    }

    public static func codes(_ x: Int, _ y: Int) -> SIMD4<Float> {
        SIMD4(Float((3 * y + x) & 255), Float((x + 7 * y) & 255), Float((4 * x + y) & 255), 255) / 255
    }

    /// Metal's BT.601 video-range conversion (YcbcrStore.converted) laid Y0 Cb Y1 Cr: the 16 whole pairs of a row take
    /// each pair's Cb and Cr from both texels, pair 16 from texel 32 alone and keeps its Y1 byte (texel 33 is outside).
    static func oddDrawn(_ before: [UInt8]) -> [UInt8] {
        let height = surfaceHeight
        let pairs = YcbcrStore.converted(
            (0 ..< (32 * height)).map { codes($0 % 32, $0 / 32) }, width: 32, height: height, full: false,
            halvedDown: false
        )
        let last = YcbcrStore.converted(
            (0 ..< height).map { codes(32, $0) }, width: 1, height: height, full: false, halvedAcross: false,
            halvedDown: false
        )
        var out = before
        for y in 0 ..< height {
            for p in 0 ..< 16 {
                let at = y * 128 + 4 * p, block = (y * 16 + p) * 2
                out[at] = UInt8(pairs.luma[y * 32 + 2 * p])
                out[at + 1] = UInt8(pairs.chroma[block])
                out[at + 2] = UInt8(pairs.luma[y * 32 + 2 * p + 1])
                out[at + 3] = UInt8(pairs.chroma[block + 1])
            }
            out[y * 128 + 64] = UInt8(last.luma[y])
            out[y * 128 + 65] = UInt8(last.chroma[2 * y])
            out[y * 128 + 67] = UInt8(last.chroma[2 * y + 1])
        }
        return out
    }

    public struct Reading: Sendable {
        /// 0 completed, 1 the command buffer failed (its facade path in `path`), 2 no texture
        public let status: Int
        public let bytesOff: Int
        public let path: String
        public let first: String
    }

    public static func surface(_ c: Case) throws -> IOSurfaceRef {
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: c.surfaceWidth, kIOSurfaceHeight: surfaceHeight, kIOSurfacePixelFormat: c.fourcc,
            kIOSurfaceBytesPerElement: c.bytesPerElement, kIOSurfaceBytesPerRow: c.pitch,
            kIOSurfaceAllocSize: c.pitch * surfaceHeight,
        ]
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else { throw TestFailure("no surface") }
        IOSurfaceLock(surface, [], nil)
        let base = IOSurfaceGetBaseAddress(surface).assumingMemoryBound(to: UInt8.self)
        for i in 0 ..< IOSurfaceGetAllocSize(surface) { base[i] = UInt8(truncatingIfNeeded: i * 31 + 7) }
        IOSurfaceUnlock(surface, [], nil)
        return surface
    }

    public static func bytes(_ surface: IOSurfaceRef) -> [UInt8] {
        IOSurfaceLock(surface, .readOnly, nil)
        defer { IOSurfaceUnlock(surface, .readOnly, nil) }
        let base = IOSurfaceGetBaseAddress(surface).assumingMemoryBound(to: UInt8.self)
        return Array(UnsafeBufferPointer(start: base, count: IOSurfaceGetAllocSize(surface)))
    }

    public static func observe(_ gpu: GPU, _ c: Case) async throws -> Reading {
        let surface = try surface(c)
        let start = bytes(surface)
        let format = MTLPixelFormat(rawValue: c.format)!
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: c.width, height: c.height, mipmapped: false
        )
        descriptor.usage = c.usage
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            return Reading(status: 2, bytesOff: 0, path: "-", first: "-")
        }
        let state = c.drawn ? try gpu.render(fragment: "texel_values", format: format) : nil
        let data = try gpu.buffer((0 ..< (c.width * c.height)).map { codes($0 % c.width, $0 / c.width) })
        let timing = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = state == nil ? .clear : .dontCare
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.5, green: 0.25, blue: 0.75, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            if let state {
                var corners = GPU.fullScreen
                var width = UInt32(c.width)
                encoder.setRenderPipelineState(state)
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentBuffer(data, offset: 0, index: 0)
                encoder.setFragmentBytes(&width, length: 4, index: 1)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
        if timing.path
            .hasPrefix("error:") { return Reading(status: 1, bytesOff: 0, path: timing.facadePath, first: "-") }
        let got = bytes(surface), want = c.expected(start)
        let wrong = (0 ..< got.count).filter { got[$0] != want[$0] }
        let first = wrong.first.map { "\($0):\(String(got[$0], radix: 16))/\(String(want[$0], radix: 16))" } ?? "-"
        return Reading(status: 0, bytesOff: wrong.count, path: timing.facadePath, first: first)
    }
}
