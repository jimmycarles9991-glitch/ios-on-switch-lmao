import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A 540 (YCbCr 4:2:0 codes) render target over plane 0 of a '420v' whose planes' rows overlap (a pitch below a row's
/// bytes): alloc 65536, plane 1 at 32768, both planes' rows `pitch` apart, every byte 0xee first; one DontCare and Store
/// pass of one draw writing Y (x + 7y) & 255 and, over each chroma block b, Cb (b.x + 5 b.y) & 255, Cr
/// (3 b.x + 11 b.y + 1) & 255 (overlap_codes). Each plane byte holds the code of the row whose store an M4 Max lands last
/// there: tiles of 64x32 luma texels, the last tile row first, each row from its last tile back, inside a tile by
/// 32-texel column, then row pair, a pair's even row after its odd one (overlap_order.h, fit to the M4 Max's runs)
/// (gpu.gm20b.ycbcr_overlap_rows).
public enum YcbcrOverlapRows {
    public struct Shape: Sendable {
        public let key: String
        let width, height, pitch: Int
    }

    public static let shapes = [
        Shape(key: "a", width: 64, height: 16, pitch: 16), Shape(key: "b", width: 64, height: 64, pitch: 16),
        Shape(key: "c", width: 128, height: 17, pitch: 32),
    ]
    static let chromaAt = 32768, allocation = 65536

    public struct Reading: Sendable {
        /// 0 completed, 1 the command buffer failed (`path`), 2 no surface or texture
        public var status = 0
        public var lumaOff = 0, chromaOff = 0, pastChanged = 0
        public var first = "-"
        public var path = "-"
    }

    /// Where the store of luma texel (x, y) lands, later stores higher
    static func order(_ x: Int, _ y: Int) -> Int {
        (0xffff - y / 32) << 14 | (0xff - x / 64) << 6 | (x % 64 / 32) << 5 | (y % 32 / 2) << 1 | (y & 1 == 0 ? 1 : 0)
    }

    /// The row whose store lands last on byte `a` of a plane of `rows` rows of `row` bytes `pitch` apart, and the byte's
    /// offset in it: a row's byte b names luma texel (b / texelBytes, row * down), a chroma pair's two bytes its first
    static func winner(
        _ a: Int, pitch: Int, row: Int, rows: Int, down: Int, texelBytes: Int = 1
    ) -> (row: Int, offset: Int)? {
        let last = min(a / pitch, rows - 1), first = a >= row ? (a - row) / pitch + 1 : 0
        guard first <= last else { return nil }
        let at = { (r: Int) in order((a - r * pitch) / texelBytes, r * down) }
        return (first ... last).max { at($0) < at($1) }.map { ($0, a - $0 * pitch) }
    }

    static func surface(_ s: Shape) -> IOSurfaceRef? {
        let planes: [[CFString: Any]] = [
            [
                kIOSurfacePlaneWidth: min(s.width, s.pitch), kIOSurfacePlaneHeight: s.height,
                kIOSurfacePlaneBytesPerElement: 1, kIOSurfacePlaneBytesPerRow: s.pitch, kIOSurfacePlaneOffset: 0,
                kIOSurfacePlaneSize: chromaAt,
            ],
            [
                kIOSurfacePlaneWidth: min(s.width / 2, s.pitch / 2), kIOSurfacePlaneHeight: (s.height + 1) / 2,
                kIOSurfacePlaneBytesPerElement: 2, kIOSurfacePlaneBytesPerRow: s.pitch, kIOSurfacePlaneOffset: chromaAt,
                kIOSurfacePlaneSize: allocation - chromaAt,
            ],
        ]
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: min(s.width, s.pitch), kIOSurfaceHeight: s.height,
            kIOSurfacePixelFormat: OSType(0x3432_3076),
            kIOSurfaceAllocSize: allocation, kIOSurfacePlaneInfo: planes,
        ]
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else { return nil }
        IOSurfaceLock(surface, [], nil)
        memset(IOSurfaceGetBaseAddress(surface), 0xee, allocation)
        IOSurfaceUnlock(surface, [], nil)
        return surface
    }

    public static func observe(_ gpu: GPU, _ s: Shape) async throws -> Reading {
        guard let surface = surface(s) else { return Reading(status: 2) }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: 540)!, width: s.width, height: s.height, mipmapped: false
        )
        descriptor.usage = s.height & 1 == 1 ? [.shaderRead] : [.shaderRead, .renderTarget]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            return Reading(status: 2)
        }
        let state = try gpu.render(fragment: "overlap_codes", format: texture.pixelFormat)
        let timing = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            var corners = GPU.fullScreen
            encoder.setRenderPipelineState(state)
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        guard !timing.path.hasPrefix("error:") else { return Reading(status: 1, path: timing.facadePath) }
        let bytes = YcbcrOnePlaneTargets.bytes(surface)
        var reading = Reading(path: timing.facadePath)
        let lumaEnd = s.pitch * (s.height - 1) + s.width, rows = (s.height + 1) / 2
        let chromaEnd = s.pitch * (rows - 1) + s.width
        for a in 0 ..< lumaEnd {
            guard let w = winner(a, pitch: s.pitch, row: s.width, rows: s.height, down: 1) else { continue }
            let want = UInt8((w.offset + 7 * w.row) & 255)
            if bytes[a] != want {
                reading.lumaOff += 1
                if reading.first == "-" { reading.first = "luma\(a):\(bytes[a])_not_\(want)_row\(w.row)" }
            }
        }
        for a in 0 ..< chromaEnd {
            guard let w = winner(a, pitch: s.pitch, row: s.width, rows: rows, down: 2) else { continue }
            let bx = w.offset / 2
            let want = UInt8((w.offset & 1 == 0 ? bx + 5 * w.row : 3 * bx + 11 * w.row + 1) & 255)
            if bytes[chromaAt + a] != want {
                reading.chromaOff += 1
                if reading.first == "-" { reading.first = "chroma\(a):\(bytes[chromaAt + a])_not_\(want)_row\(w.row)" }
            }
        }
        reading.pastChanged = (lumaEnd ..< chromaAt).filter { bytes[$0] != 0xee }.count
            + ((chromaAt + chromaEnd) ..< allocation).filter { bytes[$0] != 0xee }.count
        return reading
    }
}
