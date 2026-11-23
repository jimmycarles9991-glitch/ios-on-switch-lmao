import Foundation
import HWTestCore
import IOSurface
import Metal

/// A 540 (YCbCr 4:2:0 codes) render target turned by rotations 0 to 3 over a '420v' whose luma rows of 136 bytes lie 16
/// apart (planes 136x104, plane 1 at 16384, every byte 0xee first), one DontCare and Store pass of one draw writing Y
/// (x + 7y) & 255 from the frame's coordinates (overlap_codes) (gpu.gm20b.ycbcr_turned_overlap_order). An M4 Max stores a
/// turned target by 32x32 tiles of its FRAME: of the rows that reach a luma byte from one frame tile, only the frame
/// row in the later row pair, a pair's even row after its odd one, can keep the byte, in every run (an M4 Max's
/// turned-order probe under macOS 27.2.1: 0 misses of 1,461,824 such byte runs over 124 cases); which tile lands last
/// is a race. Judged: the bytes where some reaching row cannot win, each holding one tile's winner.
public enum YcbcrTurnedOverlapOrder {
    static let plane = (width: 136, height: 104, pitch: 16, chromaAt: 16384, allocation: 32768)
    public static let turns = [0, 1, 2, 3]

    public struct Reading: Sendable {
        public var made = false
        /// Luma bytes some reaching row cannot win, and those holding no frame tile's winner
        public var judged = 0, misses = 0
        public var first = "-"
        public var path = "-"
    }

    static func frameSize(_ turn: Int) -> (width: Int, height: Int) {
        turn & 1 == 1 ? (plane.height, plane.width) : (plane.width, plane.height)
    }

    /// The frame texel a turned target stores at plane texel (px, py)
    static func frameTexel(_ turn: Int, _ px: Int, _ py: Int) -> (x: Int, y: Int) {
        let (w, h) = frameSize(turn)
        switch turn {
        case 1: return (py, h - 1 - px)
        case 2: return (w - 1 - px, h - 1 - py)
        case 3: return (w - 1 - py, px)
        default: return (px, py)
        }
    }

    static func surface() -> IOSurfaceRef? {
        let p = plane
        let planes: [[CFString: Any]] = [
            [
                kIOSurfacePlaneWidth: p.pitch, kIOSurfacePlaneHeight: p.height, kIOSurfacePlaneBytesPerElement: 1,
                kIOSurfacePlaneBytesPerRow: p.pitch, kIOSurfacePlaneOffset: 0, kIOSurfacePlaneSize: p.pitch * p.height,
            ],
            [
                kIOSurfacePlaneWidth: p.pitch / 2, kIOSurfacePlaneHeight: p.height / 2,
                kIOSurfacePlaneBytesPerElement: 2, kIOSurfacePlaneBytesPerRow: p.pitch,
                kIOSurfacePlaneOffset: p.chromaAt, kIOSurfacePlaneSize: p.pitch * p.height / 2,
            ],
        ]
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: p.width, kIOSurfaceHeight: p.height, kIOSurfacePixelFormat: OSType(0x3432_3076),
            kIOSurfaceAllocSize: p.allocation, kIOSurfacePlaneInfo: planes,
        ]
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else { return nil }
        IOSurfaceLock(surface, [], nil)
        memset(IOSurfaceGetBaseAddress(surface), 0xee, p.allocation)
        IOSurfaceUnlock(surface, [], nil)
        return surface
    }

    public static func observe(_ gpu: GPU, turn: Int) async throws -> Reading {
        guard let surface = surface() else { throw TestFailure("no '420v' of rows 16") }
        let size = frameSize(turn)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: 540)!, width: size.width, height: size.height, mipmapped: false
        )
        descriptor.usage = [.renderTarget]
        descriptor.storageMode = .shared
        if turn != 0 {
            guard descriptor.responds(to: NSSelectorFromString("setRotation:")) else { return Reading() }
            descriptor.setValue(NSNumber(value: turn), forKey: "rotation")
            descriptor.setValue(NSNumber(value: true), forKey: "framebufferOnly")
        }
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            return Reading()
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
        var reading = Reading(made: true, path: timing.facadePath)
        guard !timing.path.hasPrefix("error:") else { return reading }
        let bytes = SurfaceReads.bytes(surface)
        let p = plane
        for a in 0 ..< (p.pitch * (p.height - 1) + p.width) {
            let last = min(a / p.pitch, p.height - 1), first = a >= p.width ? (a - p.width) / p.pitch + 1 : 0
            guard last > first else { continue }
            let texels = (first ... last).map { frameTexel(turn, a - $0 * p.pitch, $0) }
            let code = { (t: (x: Int, y: Int)) in UInt8((t.x + 7 * t.y) & 255) }
            let key = { (t: (x: Int, y: Int)) in (t.y % 32 / 2) << 1 | (t.y & 1 == 0 ? 1 : 0) }
            let tiles = Dictionary(grouping: texels) { ($0.x / 32) << 16 | ($0.y / 32) }
            let winners = Set(tiles.values.map { code($0.max { key($0) < key($1) }!) })
            guard !Set(texels.map(code)).isSubset(of: winners) else { continue }
            reading.judged += 1
            guard !winners.contains(bytes[a]) else { continue }
            reading.misses += 1
            if reading.first == "-" { reading.first = "\(a):\(bytes[a])" }
        }
        return reading
    }
}
