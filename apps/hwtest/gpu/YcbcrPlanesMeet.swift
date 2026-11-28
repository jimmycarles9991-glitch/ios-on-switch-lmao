import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// gpu.gm20b.ycbcr_planes_meet: a YCBCR8_420_2P (500) 8x8 target over a '420v' of 8 MiB + 16 KiB whose planes' stored
/// rows meet (luma rows 8 at 0, chroma rows 8 at 64: both stored 16 bytes apart, so chroma row k lands on luma row
/// 4 + k), every byte 0xa5, drawn whole with a colour of each pixel's own. An M4 Max keeps the later 2x2 quad's byte in
/// a 32x32 frame tile and ORs the bytes of one quad's two planes: luma row 4 + k wins over chroma row k for k < 3, and
/// at k = 3 (one quad row) each byte is the OR of both. Each writer's own byte is the same pass's on a control of rows 16
/// and 16 with the planes apart.
public enum YcbcrPlanesMeet {
    typealias P = SurfaceReads.Plane
    static let fill: UInt8 = 0xa5

    public struct Reading: Sendable {
        public var made = false
        public var path = "-"
        /// Bytes off Apple's rule: where the planes meet, and everywhere else
        public var meetOff = -1
        public var otherOff = -1
    }

    static func target(_ gpu: GPU, _ planes: [P]) -> (IOSurfaceRef, MTLTexture)? {
        let usage: MTLTextureUsage = [.shaderRead, .renderTarget]
        guard let surface = RowsUnder16.surface("420v", 8, 8, planes: planes, fill: fill),
              let texture = SurfaceReads.texture(gpu, 500, 8, 8, over: surface, usage: usage)
        else { return nil }
        return (surface, texture)
    }

    public static func observe(_ gpu: GPU) async throws -> Reading {
        let meeting = [P(at: 0, 8, 8, bpe: 1, rows: 8), P(at: 64, 4, 4, bpe: 2, rows: 8)]
        let apart = [P(at: 0, 8, 8, bpe: 1, rows: 16), P(at: 4096, 4, 4, bpe: 2, rows: 16)]
        guard let (control, controlTarget) = target(gpu, apart) else { throw TestFailure("no 500 of rows 16 and 16") }
        let drawn = try await RowsUnder16.pass(
            gpu, controlTarget, clear: YcbcrRowsUnder16Stores.clear, fragment: "rows16_ycbcr_colours"
        )
        guard !drawn.hasPrefix("error") else { throw TestFailure("the control's pass failed: \(drawn)") }
        let own = RowsUnder16.bytes(control, at: Array(0 ..< 4096 + 64))
        let luma = { (x: Int, y: Int) in own[16 * y + x] }, chroma = { (k: Int, b: Int) in own[4096 + 16 * k + b] }
        guard let (surface, texture) = target(gpu, meeting) else { return Reading() }
        let path = try await RowsUnder16.pass(
            gpu, texture, clear: YcbcrRowsUnder16Stores.clear, fragment: "rows16_ycbcr_colours"
        )
        var want = [UInt8](repeating: fill, count: 16384)
        for y in 0 ..< 8 { for x in 0 ..< 8 { want[16 * y + x] = luma(x, y) } }
        var meet: Set<Int> = []
        for k in 0 ..< 4 {
            for b in 0 ..< 8 {
                let at = 64 + 16 * k + b
                meet.insert(at)
                want[at] = k < 3 ? luma(b, 4 + k) : luma(b, 7) | chroma(k, b)
            }
        }
        let got = RowsUnder16.bytes(surface, at: Array(0 ..< 16384))
        let wrong = (0 ..< 16384).filter { got[$0] != want[$0] }
        let past = RowsUnder16.off(surface, got, fill: fill)
        return Reading(
            made: true, path: path, meetOff: wrong.filter { meet.contains($0) }.count,
            otherOff: wrong.filter { !meet.contains($0) }.count + past
        )
    }
}
