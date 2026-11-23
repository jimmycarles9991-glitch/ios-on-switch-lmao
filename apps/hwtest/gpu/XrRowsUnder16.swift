import Foundation
import HWTestCore
import IOSurface
import Metal

/// RGB10A8_2P_XR10 (550) 2x8 over a 'b3a8' whose planes' rows are under 16 bytes (gpu.gm20b.xr_rows_under_16): plane 0
/// rows 8 at 0, plane 1 rows 2 at 64, 8 MiB + 16 KiB. An M4 Max reads texel (x, y)'s word at y MiB + 4x and its alpha
/// at 64 + y MiB + x, and stores them 16 bytes a row apart from each plane's offset, a word row overrunning the alpha
/// rows it meets.
public enum XrRowsUnder16 {
    typealias P = SurfaceReads.Plane
    static let planes = [P(at: 0, 2, 8, bpe: 4, rows: 8), P(at: 64, 2, 8, bpe: 1, rows: 2)]
    static let usage: MTLTextureUsage = [.shaderRead, .shaderWrite, .renderTarget]
    static let clear = MTLClearColor(red: 0.5, green: 0.25, blue: -0.25, alpha: 0.75)
    static let clearWord: UInt32 = 0x27f8_0100, clearAlpha: UInt8 = 0xbf

    public struct Reading: Sendable {
        public var made = false
        /// Texels a kernel reads off the read rule, over a patterned surface
        public var readOff = -1
        /// Bytes of the surface a clear pass leaves off the store rule
        public var clearOff = -1
        /// After the clear, texels read off the read rule over the cleared bytes, and those that read the clear
        public var readAfterOff = -1
        public var readAfterClear = -1
        /// Bytes a whole-target coded draw and a coded kernel store leave off the store rule (the words land last where
        /// word and alpha rows meet: the kernel's sixteen threads are one simdgroup, whose later quadgroup wins)
        public var drawOff = -1
        public var kernelOff = -1
        public var failures: [String] = []
    }

    /// Texel i = 2y + x's word and alpha byte as rows16_xr_codes and rows16_xr_store write them
    static func coded(_ x: Int, _ y: Int) -> (word: UInt32, alpha: UInt8) {
        let i = UInt32(2 * y + x)
        return ((300 + 11 * i) << 20 | (520 + 7 * i) << 10 | (100 + 13 * i), UInt8(16 + 15 * i))
    }

    static func texture(_ gpu: GPU, fill: UInt8) -> (IOSurfaceRef, MTLTexture)? {
        guard let surface = RowsUnder16.surface("b3a8", 2, 8, planes: planes, fill: fill),
              let texture = SurfaceReads.texture(gpu, 550, 2, 8, over: surface, usage: usage)
        else { return nil }
        return (surface, texture)
    }

    /// Texels of a kernel read off the read rule over the surface's bytes now (half a code either way: Apple's XR
    /// decode is not correctly rounded), and those that read `colour`
    static func readOff(_ gpu: GPU, _ surface: IOSurfaceRef, _ texture: MTLTexture, colour: SIMD4<Float>? = nil)
        async throws -> (off: Int, matching: Int)
    {
        let got = try await SurfaceReads.read(gpu, texture)
        let row = RowsUnder16.mebibyte
        let offsets = (0 ..< 8).flatMap { y in (0 ..< 8).map { y * row + $0 } + [64 + y * row, 65 + y * row] }
        let bytes = RowsUnder16.bytes(surface, at: offsets)
        let off = SurfaceReads.off(got, tolerance: 0.5 / 510) { i in
            let (x, y) = (i % 2, i / 2)
            return SurfaceReads.xr(SurfaceReads.word(bytes, 10 * y + 4 * x), alpha: Float(bytes[10 * y + 8 + x]) / 255)
        }
        let near = { (t: SIMD4<Float>, c: SIMD4<Float>) in (0 ..< 4).allSatisfy { abs(t[$0] - c[$0]) <= 0.5 / 510 } }
        let matching = colour.map { c in got.filter { near($0, c) }.count }
        return (off, matching ?? 0)
    }

    public static func observe(_ gpu: GPU) async throws -> Reading {
        var r = Reading()
        guard let (patterned, readable) = texture(gpu, fill: 0) else { return r }
        r.made = true
        RowsUnder16.edit(patterned) { base in
            for y in 0 ..< 8 {
                for b in 0 ..< 128 { base[y * RowsUnder16.mebibyte + b] = SurfaceReads.pattern(y * 131 + b) }
            }
        }
        r.readOff = try await readOff(gpu, patterned, readable).off

        guard let (cleared, target) = texture(gpu, fill: 0xff) else { throw TestFailure("no second 550") }
        let cleared16 = try await RowsUnder16.pass(gpu, target, clear: clear, fragment: nil)
        if cleared16.hasPrefix("error") { r.failures.append("clear \(cleared16)") }
        var want = [UInt8](repeating: 0xff, count: RowsUnder16.head)
        for y in 0 ..< 8 { for x in 0 ..< 2 { RowsUnder16.put(clearWord, &want, at: 16 * y + 4 * x) } }
        for k in 0 ..< 4 { for x in 0 ..< 2 { want[128 + 16 * k + x] = clearAlpha } }
        r.clearOff = RowsUnder16.off(cleared, want, fill: 0xff)
        let colour = SIMD4<Float>(SurfaceReads.xr(clearWord, alpha: 0))
        (r.readAfterOff, r.readAfterClear) = try await readOff(gpu, cleared, target, colour: colour)

        var stored = [UInt8](repeating: 0xff, count: RowsUnder16.head)
        for y in 0 ..< 8 { for x in 0 ..< 2 { stored[64 + 16 * y + x] = coded(x, y).alpha } }
        for y in 0 ..< 8 { for x in 0 ..< 2 { RowsUnder16.put(coded(x, y).word, &stored, at: 16 * y + 4 * x) } }
        guard let (drawn, drawTarget) = texture(gpu, fill: 0xff) else { throw TestFailure("no third 550") }
        let draw = try await RowsUnder16.pass(gpu, drawTarget, clear: clear, fragment: "rows16_xr_codes")
        if draw.hasPrefix("error") { r.failures.append("draw \(draw)") }
        r.drawOff = RowsUnder16.off(drawn, stored, fill: 0xff)

        guard let (written, kernelTarget) = texture(gpu, fill: 0xff) else { throw TestFailure("no fourth 550") }
        let store = try await RowsUnder16.dispatch(gpu, "rows16_xr_store", kernelTarget)
        if store.hasPrefix("error") { r.failures.append("kernel \(store)") }
        r.kernelOff = RowsUnder16.off(written, stored, fill: 0xff)
        return r
    }
}
