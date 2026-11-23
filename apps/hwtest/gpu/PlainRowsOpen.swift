import Foundation
import HWTestCore
import Metal

/// The plain-rows stores an M4 Max keeps in an order of its own (gpu.gm20b.plain_rows_blit_tiles, which the driver
/// takes for whole copies into 8- and 16-byte texels: overlap_order.h copyOrder; gpu.gm20b.plain_rows_quarter_turns,
/// which no fixed order reproduces, its frame tiles stored concurrently), each over RGBA32Uint 40x40 on rows of 128
/// bytes: eight texels a row apart, so elements 8 to 343 are reached by two to five rows and keep one.
extension PlainRows {
    public static let quarterShape = Shape(MTLPixelFormat.rgba32Uint.rawValue, 40, 40, bpp: 16, pitch: 128)

    /// Where a copy's store of texel (x, y) lands, later higher: 32x32 tiles from the last tile row up, each row from
    /// its last tile back; in a tile the 2x2 quad's row, then its column, the even row of a quad after the odd (an M4
    /// Max's copies from a buffer into 8- and 16-byte texels, every stable element of ten runs)
    public static func copyTileOrder(_ x: Int, _ y: Int) -> Int {
        (0x3ff - y / 32) << 18 | (0xff - x / 32) << 10 | (y % 32 / 2) << 5 | (x % 32 / 2) << 1 | (y % 2 == 0 ? 1 : 0)
    }

    /// Element `a`'s bytes after a copy of every texel's code into `s` from a buffer
    public static func copied(_ s: Shape, _ a: Int) -> [UInt8] {
        let rows = s.rows(reaching: a)
        let order = { (r: Int) in copyTileOrder((a - s.stride * r) / s.bpp, r) }
        guard let r = rows.max(by: { order($0) < order($1) }) else { return [UInt8](repeating: 0xee, count: s.bpp) }
        return code(s.bpp, (a - s.stride * r) / s.bpp, r)
    }

    /// The rows ('0' + row) an M4 Max's pass of codes over the whole frame of quarterShape turned by 1 and by 3 leaves
    /// in elements 8 to 343, the same in each of ten runs
    public static let quarterRows: [Int: String] = [
        1: "00000000111111112222222233333333444444445555555566666666777777778888888899999999::::::::;;;;;;;;<<<<<<<<"
            + "========>>>>>>>>????????@@@@@@@@AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDDEEEEEEEEFFFFFFFFGGGGGGGGHHHHHHHHIIIIIIII"
            + "JJJJJJJJKKKKKKKKLLLLLLLLMMMMMMMMNNNNNNNNOOOOOOOOPPPPPPPPQQQQQQQQRRRRRRRRSSSSSSSSTTTTTTTTUUUUUUUUVVVVVVVV"
            + "WWWWWWWWWWWWWWWWWWWWWWWW",
        3: "000000000000000000000000111111112222222233333333444444445555555566666666777777778888888899999999::::::::"
            + ";;;;;;;;<<<<<<<<========>>>>>>>>????????@@@@@@@@AAAAAAAABBBBBBBBCCCCCCCCDDDDDDDDEEEEEEEEFFFFFFFFGGGGGGGG"
            + "HHHHHHHHIIIIIIIIJJJJJJJJKKKKKKKKLLLLLLLLMMMMMMMMNNNNNNNNOOOOOOOOPPPPPPPPQQQQQQQQRRRRRRRRSSSSSSSSTTTTTTTT"
            + "UUUUUUUUVVVVVVVVWWWWWWWW",
    ]

    /// Element `a`'s bytes after quarterShape's pass turned by `turn`: the one row reaching it, or the row quarterRows
    /// names
    public static func turned(_ turn: Int, _ a: Int) -> [UInt8] {
        let s = quarterShape, rows = s.rows(reaching: a)
        guard rows.count > 1, let held = quarterRows[turn] else {
            let only = rows.first.map { code(s.bpp, (a - s.stride * $0) / s.bpp, $0) }
            return only ?? [UInt8](repeating: 0xee, count: s.bpp)
        }
        let r = Int(held.utf8[held.utf8.index(held.utf8.startIndex, offsetBy: a / s.bpp - 8)]) - 48
        return code(s.bpp, (a - s.stride * r) / s.bpp, r)
    }
}
