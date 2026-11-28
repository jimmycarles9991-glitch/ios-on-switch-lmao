import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A 504 (one-plane 4:4:4 10-bit words) render target of 96 x h over a 'v410' whose rows overlap (pitch below a row's 384
/// bytes), every byte 0xee first, one DontCare and Store pass drawing overlap_hash, against the same pass into a 'v410' of
/// 512-byte rows: each overlapped byte holds the reference's byte of the row whose store an M4 Max lands last there
/// (YcbcrOverlapRows.winner, a word a texel) (gpu.gm20b.ycbcr_overlap_words).
public enum YcbcrOverlapWords {
    public struct Shape: Sendable {
        public let key: String
        let height, pitch: Int
    }

    public static let shapes = [Shape(key: "p256", height: 33, pitch: 256), Shape(key: "p320", height: 64, pitch: 320)]
    static let width = 96, apart = 512

    public struct Reading: Sendable {
        public var status = 0
        public var bytesOff = 0, pastChanged = 0
        public var first = "-"
        public var path = "-"
    }

    static func surface(height: Int, pitch: Int) -> IOSurfaceRef? {
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: min(width, pitch / 4), kIOSurfaceHeight: height,
            kIOSurfacePixelFormat: OSType(0x7634_3130),
            kIOSurfaceBytesPerElement: 4, kIOSurfaceBytesPerRow: pitch, kIOSurfaceAllocSize: pitch * height + 4096,
        ]
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else { return nil }
        IOSurfaceLock(surface, [], nil)
        memset(IOSurfaceGetBaseAddress(surface), 0xee, IOSurfaceGetAllocSize(surface))
        IOSurfaceUnlock(surface, [], nil)
        return surface
    }

    /// The pass into a 504 over `surface`: the facade's path, nil without a texture
    static func draw(_ gpu: GPU, _ surface: IOSurfaceRef, height: Int) async throws -> GPU.Timing? {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: 504)!, width: width, height: height, mipmapped: false
        )
        descriptor.usage = [.shaderRead, .renderTarget]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0)
        else { return nil }
        let state = try gpu.render(fragment: "overlap_hash", format: texture.pixelFormat)
        return try await gpu.run(tolerateError: true) { commands in
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
    }

    public static func observe(_ gpu: GPU, _ s: Shape) async throws -> Reading {
        guard let overlapped = surface(height: s.height, pitch: s.pitch), let apart = surface(
            height: s.height,
            pitch: apart
        )
        else { return Reading(status: 2) }
        guard let timing = try await draw(gpu, overlapped, height: s.height), let reference = try await draw(
            gpu,
            apart,
            height: s.height
        )
        else { return Reading(status: 2) }
        for t in [timing, reference] where t.path.hasPrefix("error:") { return Reading(status: 1, path: t.facadePath) }
        let got = YcbcrOnePlaneTargets.bytes(overlapped), want = YcbcrOnePlaneTargets.bytes(apart)
        var reading = Reading(path: timing.facadePath)
        let row = width * 4, end = s.pitch * (s.height - 1) + row
        for a in 0 ..< end {
            guard let w = YcbcrOverlapRows.winner(a, pitch: s.pitch, row: row, rows: s.height, down: 1, texelBytes: 4)
            else { continue }
            let expected = want[w.row * Self.apart + w.offset]
            if got[a] != expected {
                reading.bytesOff += 1
                if reading.first == "-" { reading.first = "\(a):\(got[a])_not_\(expected)_row\(w.row)" }
            }
        }
        reading.pastChanged = (end ..< got.count).filter { got[$0] != 0xee }.count
        return reading
    }
}
