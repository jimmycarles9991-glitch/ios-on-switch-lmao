import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// CPU copies of two-plane YCbCr textures over '420v' planes of rows under 16 bytes in 8 MiB + 16 KiB
/// (gpu.gm20b.ycbcr_cpu_copies): a 500 8x8 over plane 0 of rows 8 at offset 8 (plane 1 of rows 8 at 4100), every byte
/// 0xee. An M4 Max's replaceRegion and getBytes move the luma plane alone, one code a texel, at the plane's exact
/// offset and its stated rows; of a packed 10-bit 508 they move nothing; with a bytesPerRow under the luma row nothing
/// moves.
public enum YcbcrCpuCopies {
    typealias P = SurfaceReads.Plane
    static let region = MTLRegionMake2D(2, 3, 4, 4)
    static let fill: UInt8 = 0xee, sentinel: UInt8 = 0x55

    public struct Reading: Sendable {
        public var made = false
        /// Surface bytes off after replaceRegion of (2,3) 4x4 at bytesPerRow 16
        public var writeOff = -1
        /// Buffer bytes off after getBytes of the same region, its codes set 200 - (16y + x) by the CPU, into 0x55
        public var readOff = -1
        public var packedMade = false
        /// Surface and buffer bytes a 508's replaceRegion and getBytes changed
        public var packedMoved = -1
        /// Surface bytes a replaceRegion of bytesPerRow 2, under its 4-byte row, changed
        public var shortRowsMoved = -1
    }

    /// Code c[y][x] of the region, (16y + x) * 7 + 3, at byte 16y + x of a 64-byte source; the rest 0x99
    static let codes: [UInt8] = (0 ..< 64).map { i in i % 16 < 4 ? UInt8(truncatingIfNeeded: i * 7 + 3) : 0x99 }

    static func surface(luma: Int, at offset: Int, chroma: Int, at chromaOffset: Int) -> IOSurfaceRef? {
        RowsUnder16.surface(
            "420v", 8, 8,
            planes: [P(at: offset, 8, 8, bpe: 1, rows: luma), P(at: chromaOffset, 4, 4, bpe: 2, rows: chroma)],
            fill: fill
        )
    }

    static func replace(_ texture: MTLTexture, rows: Int) {
        codes.withUnsafeBytes {
            texture.replace(region: region, mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: rows)
        }
    }

    public static func observe(_ gpu: GPU) throws -> Reading {
        var r = Reading()
        guard let written = surface(luma: 8, at: 8, chroma: 8, at: 4100),
              let short = surface(luma: 8, at: 8, chroma: 8, at: 4100),
              let packed = surface(luma: 16, at: 0, chroma: 16, at: 4096)
        else { throw TestFailure("no '420v' 8x8 of rows 8") }
        guard let luma = SurfaceReads.texture(gpu, 500, 8, 8, over: written),
              let shortRows = SurfaceReads.texture(gpu, 500, 8, 8, over: short)
        else { return r }
        r.made = true
        replace(luma, rows: 16)
        var want = [UInt8](repeating: fill, count: 16384)
        for y in 0 ..< 4 { for x in 0 ..< 4 { want[8 + (3 + y) * 8 + 2 + x] = codes[16 * y + x] } }
        r.writeOff = RowsUnder16.off(written, want, fill: fill)

        RowsUnder16.edit(written) { base in
            for y in 0 ..< 4 { for x in 0 ..< 4 { base[8 + (3 + y) * 8 + 2 + x] = UInt8(200 - (16 * y + x)) } }
        }
        var read = [UInt8](repeating: sentinel, count: 64)
        read.withUnsafeMutableBytes { luma.getBytes($0.baseAddress!, bytesPerRow: 16, from: region, mipmapLevel: 0) }
        let expected = (0 ..< 64).map { (i: Int) -> UInt8 in i % 16 < 4 ? UInt8(200 - i / 16 * 16 - i % 16) : sentinel }
        r.readOff = zip(read, expected).filter { $0 != $1 }.count

        replace(shortRows, rows: 2)
        r.shortRowsMoved = RowsUnder16.off(short, [UInt8](repeating: fill, count: 16384), fill: fill)

        guard let tenBit = SurfaceReads.texture(gpu, 508, 8, 8, over: packed) else { return r }
        r.packedMade = true
        replace(tenBit, rows: 16)
        var got = [UInt8](repeating: sentinel, count: 64)
        got.withUnsafeMutableBytes { tenBit.getBytes($0.baseAddress!, bytesPerRow: 16, from: region, mipmapLevel: 0) }
        r.packedMoved = RowsUnder16.off(packed, [UInt8](repeating: fill, count: 16384), fill: fill)
            + got.filter { $0 != sentinel }.count
        return r
    }
}
