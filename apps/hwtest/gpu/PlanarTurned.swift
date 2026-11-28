import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Turned RGB10A8_2P_XR10 (550, 551) and YCbCr (500) render targets of a 32x64 frame over 64x64 planes
/// (gpu.gm20b.planar_turned): one pass each, every surface byte 0xee first, a Clear of (0.25, 0.5, 0.75, 1) and a draw of
/// (0.75, 0.25, 0.5, 0.6) over frame pixels (0, 0)-(16, 16). Against the same pass into the format unturned over a
/// 32x64 surface: an M4 Max stores frame texel (x, y) at plane texel (63 - y, x) for rotations 1 and 4, (y, 31 - x) for
/// 3, (31 - x, 63 - y) for 2, each plane's bytes the unturned pass's (words, alpha bytes, luma, a 2x2 block's chroma),
/// and no byte past the turned frame.
public enum PlanarTurned {
    typealias P = SurfaceReads.Plane

    public struct Case: Sendable {
        public let key: String
        let format: UInt
        let turn: Int
        let fourcc: String
        /// The 64x64 planes and the 32x64 reference's
        let planes: [P], reference: [P]
        let allocation: Int, referenceAllocation: Int
    }

    static let frame = (width: 32, height: 64)
    static let xrPlanes = [P(at: 0, 64, 64, bpe: 4, rows: 256), P(at: 16384, 64, 64, bpe: 1, rows: 64)]
    static let xrReference = [P(at: 0, 32, 64, bpe: 4, rows: 128), P(at: 8192, 32, 64, bpe: 1, rows: 32)]
    static let wordPlane = [P(at: 0, 64, 64, bpe: 4, rows: 256)], wordReference = [P(at: 0, 32, 64, bpe: 4, rows: 128)]

    public static let cases = [
        Case(
            key: "b3a8_550_r1", format: 550, turn: 1, fourcc: "b3a8", planes: xrPlanes, reference: xrReference,
            allocation: 20480, referenceAllocation: 10240
        ),
        Case(
            key: "b3a8_550_r3", format: 550, turn: 3, fourcc: "b3a8", planes: xrPlanes, reference: xrReference,
            allocation: 20480, referenceAllocation: 10240
        ),
        Case(
            key: "w30r_550_r2", format: 550, turn: 2, fourcc: "w30r", planes: wordPlane, reference: wordReference,
            allocation: 16384, referenceAllocation: 8192
        ),
        Case(
            key: "w30r_551_r4", format: 551, turn: 4, fourcc: "w30r", planes: wordPlane, reference: wordReference,
            allocation: 16384, referenceAllocation: 8192
        ),
        Case(
            key: "420v_500_r1", format: 500, turn: 1, fourcc: "420v",
            planes: [P(at: 0, 64, 64, bpe: 1, rows: 64), P(at: 4096, 32, 32, bpe: 2, rows: 64)],
            reference: [P(at: 0, 32, 64, bpe: 1, rows: 32), P(at: 2048, 16, 32, bpe: 2, rows: 32)], allocation: 6144,
            referenceAllocation: 3072
        ),
    ]

    public struct Reading: Sendable {
        public var made = false
        /// Frame texels whose bytes in some plane are not the unturned pass's
        public var texelsOff = 0
        /// Bytes past the turned frame that changed
        public var outside = 0
        /// Frame texels the unturned pass left 0xee in plane 0 (its pass stored nothing there)
        public var referenceUnstored = 0
        public var first = "-"
        public var paths: [String] = []
    }

    static let blank: UInt8 = 0xee

    /// Where frame texel (x, y) of a 32x64 frame lands in the turned plane
    public static func stored(_ turn: Int, _ x: Int, _ y: Int) -> (x: Int, y: Int) {
        let (w, h) = (frame.width, frame.height)
        switch turn {
        case 2: return (w - 1 - x, h - 1 - y)
        case 3: return (y, w - 1 - x)
        default: return (h - 1 - y, x)
        }
    }

    static func target(_ gpu: GPU, _ c: Case, _ surface: IOSurfaceRef, turn: Int) -> MTLTexture? {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: c.format)!, width: frame.width, height: frame.height,
            mipmapped: false
        )
        descriptor.usage = [.renderTarget]
        descriptor.storageMode = .shared
        if turn != 0 {
            guard descriptor.responds(to: NSSelectorFromString("setRotation:")) else { return nil }
            descriptor.setValue(NSNumber(value: turn), forKey: "rotation")
            descriptor.setValue(NSNumber(value: true), forKey: "framebufferOnly")
        }
        return gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0)
    }

    static func pass(_ gpu: GPU, _ texture: MTLTexture) async throws -> String {
        let state = try gpu.render(fragment: "solid_fragment", format: texture.pixelFormat)
        return try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            GPU.draw(
                encoder, corners: GPU.pixelQuad(0, 0, 16, 16, width: frame.width, height: frame.height),
                color: SIMD4(0.75, 0.25, 0.5, 0.6)
            )
            encoder.endEncoding()
        }.facadePath
    }

    /// Plane k's bytes of texel (x, y) of a surface's planes: (offset, count); a 4:2:0 chroma plane's two bytes a 2x2
    /// block
    static func at(_ planes: [(offset: Int, pitch: Int)], _ k: Int, _ x: Int, _ y: Int, ycbcr: Bool, word: Int)
        -> (Int, Int)
    {
        if k == 0 { return (planes[0].offset + y * planes[0].pitch + x * word, word) }
        if ycbcr { return (planes[1].offset + (y / 2) * planes[1].pitch + (x / 2) * 2, 2) }
        return (planes[1].offset + y * planes[1].pitch + x, 1)
    }

    static func layout(_ surface: IOSurfaceRef) -> [(offset: Int, pitch: Int)] {
        let base = Int(bitPattern: IOSurfaceGetBaseAddress(surface))
        return (0 ..< IOSurfaceGetPlaneCount(surface)).map {
            (
                Int(bitPattern: IOSurfaceGetBaseAddressOfPlane(surface, $0)) - base,
                IOSurfaceGetBytesPerRowOfPlane(surface, $0)
            )
        }
    }

    public static func observe(_ gpu: GPU, _ c: Case) async throws -> Reading {
        let (w, h) = (frame.width, frame.height)
        guard
            let surface = SurfaceReads.surface(
                c.fourcc, 64, 64, planes: c.planes, allocation: c.allocation, fill: { _ in blank }
            ),
            let plain = SurfaceReads.surface(
                c.fourcc, w, h, planes: c.reference, allocation: c.referenceAllocation, fill: { _ in blank }
            )
        else { throw TestFailure("no '\(c.fourcc)' surfaces for \(c.key)") }
        guard let reference = target(gpu, c, plain, turn: 0) else { throw TestFailure("no unturned \(c.format)") }
        var reading = Reading()
        guard let turned = target(gpu, c, surface, turn: c.turn) else { return reading }
        reading.made = true
        reading.paths = [try await pass(gpu, reference), try await pass(gpu, turned)]
        let want = SurfaceReads.bytes(plain), got = SurfaceReads.bytes(surface)
        let ycbcr = c.format == 500, word = ycbcr ? 1 : 4
        let from = layout(plain), to = layout(surface)
        var inside = [Bool](repeating: false, count: got.count)
        for y in 0 ..< h {
            for x in 0 ..< w {
                let p = stored(c.turn, x, y)
                var wrong = false
                for k in 0 ..< to.count {
                    let (a, n) = at(from, k, x, y, ycbcr: ycbcr, word: word)
                    let (b, _) = at(to, k, p.x, p.y, ycbcr: ycbcr, word: word)
                    if k == 0 && want[a ..< (a + n)].allSatisfy({ $0 == blank }) { reading.referenceUnstored += 1 }
                    for i in 0 ..< n { inside[b + i] = true }
                    wrong = wrong || !want[a ..< (a + n)].elementsEqual(got[b ..< (b + n)])
                }
                guard wrong else { continue }
                reading.texelsOff += 1
                if reading.first == "-" { reading.first = "(\(x),\(y))@(\(p.x),\(p.y))" }
            }
        }
        reading.outside = got.indices.filter { !inside[$0] && got[$0] != blank }.count
        return reading
    }
}
