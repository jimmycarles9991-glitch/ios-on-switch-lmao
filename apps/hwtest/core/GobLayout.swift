import Foundation

/// GM20B's block-linear layout of a 4-byte-texel surface (TegraGM20BTiled.cpp, TegraGM20BLayoutFit.h): 64-byte by
/// 8-row GOBs, 2^yLog2 GOBs to a block, blocks across the 64-byte-aligned row.
public enum GobLayout {
    /// The block height GM20B decides for a surface `height` rows tall
    public static func blockHeightLog2(_ height: Int) -> Int {
        let y = height >= 86 ? 4 : height >= 43 ? 3 : height >= 22 ? 2 : height >= 11 ? 1 : 0
        var clamp = 0
        while (1 << clamp) < (height + 7) / 8 { clamp += 1 }
        return min(y, clamp)
    }

    public static func offset(_ rowBytes: Int, _ yLog2: Int, _ xB: Int, _ y: Int) -> Int {
        let blockRows = 8 << yLog2
        let block = (y / blockRows) * (rowBytes / 64) + xB / 64
        let inGob =
            ((xB % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xB % 32) / 16) * 32 + (y % 2) * 16 + (xB % 16)
        return block * (512 << yLog2) + ((y % blockRows) / 8) * 512 + inGob
    }

    /// NIL's Fermi GOB (two 32-byte halves of 8 lines, its 16-byte line order) in blocks of 2^yLog2 GOBs, which a
    /// decode that took the Fermi layout writes instead of GM20B's Tegra GOBs
    public static func fermiOffset(_ rowBytes: Int, _ yLog2: Int, _ xB: Int, _ y: Int) -> Int {
        let lines = [[0x20, 0x30, 0x00, 0x10, 0xe0, 0xf0, 0xc0, 0xd0], [0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0, 0xb0]]
        let blockRows = 8 << yLog2
        let block = (y / blockRows) * (rowBytes / 64) + xB / 64
        let inGob = ((xB % 64) / 32) * 256 + lines[(xB % 32) / 16][y % 8] + xB % 16
        return block * (512 << yLog2) + ((y % blockRows) / 8) * 512 + inGob
    }

    public static func blockRows(_ height: Int, _ yLog2: Int) -> Int {
        let block = 8 << yLog2
        return (height + block - 1) / block * block
    }
}

/// Which arrangement an engine's writes into a surface match, for a read that came out wrong (NVJPG's decode into a
/// block-linear destination: HW Test gpu.iosurface.cross_process `*_explained`): the picture written each candidate
/// way, read back through the blocks GM20B decided, against what the read got. Candidates: rows at the surface's
/// 64-byte pitch, at the bare width and at a 256-byte pitch; GOB blocks of 2^0 to 2^5 GOBs, as GM20B's Tegra GOBs and
/// as NIL's Fermi GOBs; 16-byte by 16-row tiles. The answer is `<candidate>:<bytes off>` for the closest, so a near
/// miss is told from no match.
public enum JpegLayoutGuess {
    /// A 4-byte-texel surface's size and allocation, which bounds the block height GM20B can give it
    public struct Surface {
        public let width: Int
        public let height: Int
        public let allocation: Int

        public init(width: Int, height: Int, allocation: Int) {
            self.width = width
            self.height = height
            self.allocation = allocation
        }
    }

    public struct Candidate {
        public let name: String
        public let offset: (Int, Int) -> Int
    }

    public static func candidates(width: Int, rowBytes: Int) -> [Candidate] {
        let bare = width * 4, wide = (width * 4 + 255) & ~255
        var list = [
            Candidate(name: "rows") { xB, y in y * rowBytes + xB },
            Candidate(name: "rows_bare") { xB, y in y * bare + xB },
            Candidate(name: "rows_256") { xB, y in y * wide + xB },
        ]
        for yLog2 in 0 ... 5 {
            list.append(Candidate(name: "y\(yLog2)") { xB, y in GobLayout.offset(rowBytes, yLog2, xB, y) })
        }
        for yLog2 in 0 ... 5 {
            list.append(
                Candidate(name: "fermi_y\(yLog2)") { xB, y in GobLayout.fermiOffset(rowBytes, yLog2, xB, y) }
            )
        }
        list.append(
            Candidate(name: "tiled16") { xB, y in
                ((y / 16) * (rowBytes / 16) + xB / 16) * 256 + (y % 16) * 16 + xB % 16 }
        )
        return list
    }

    /// `got`: the read's rows, `surface.width` 4-byte texels each; `picture`: the byte the decode should leave at (x, y, c);
    /// `tolerance`: how far a byte may be and still match.
    public static func best(
        _ got: [UInt8], _ surface: Surface, tolerance: Int, picture: (Int, Int, Int) -> UInt8
    ) -> String {
        let (width, height) = (surface.width, surface.height)
        let rowBytes = (width * 4 + 63) & ~63
        var decided = GobLayout.blockHeightLog2(height)
        while decided > 0, rowBytes * GobLayout.blockRows(height, decided) > surface.allocation { decided -= 1 }
        let span = max(rowBytes, (width * 4 + 255) & ~255) * GobLayout.blockRows(height, 5)
        var best = ("-", Int.max)
        for candidate in candidates(width: width, rowBytes: rowBytes) {
            var memory = [UInt8](repeating: 0, count: span)
            for y in 0 ..< height {
                for xB in 0 ..< width * 4 {
                    let at = candidate.offset(xB, y)
                    if at < span { memory[at] = picture(xB / 4, y, xB % 4) }
                }
            }
            var wrong = 0
            for y in 0 ..< height {
                for xB in 0 ..< width * 4 {
                    let at = GobLayout.offset(rowBytes, decided, xB, y)
                    let read = at < span ? Int(memory[at]) : 0
                    if abs(read - Int(got[y * width * 4 + xB])) > tolerance { wrong += 1 }
                }
            }
            if wrong < best.1 { best = (candidate.name, wrong) }
        }
        return "\(best.0):\(best.1)"
    }
}
