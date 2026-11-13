import Foundation

/// An ASTC 4x4 image whose every block is an LDR void-extent block of its own colour, so a sampled texel names the block
/// it came from. Colours are 8-bit values widened to UNORM16 (c * 257), exact in both of ASTC's decode modes.
public struct AstcBlockGrid: Sendable {
    public static let blockBytes = 16
    public let across: Int
    public let down: Int

    public init(across: Int, down: Int) {
        precondition(across <= 32 && down <= 32, "one 8-bit channel step per block")
        self.across = across
        self.down = down
    }

    public var texelWidth: Int { across * 4 }
    public var texelHeight: Int { down * 4 }
    public var rowBytes: Int { across * Self.blockBytes }

    public static func colour(_ x: Int, _ y: Int) -> [UInt8] {
        [UInt8(4 + 8 * x), UInt8(4 + 8 * y), UInt8(4 + 8 * (x ^ y)), 255]
    }

    public static func voidExtent(_ rgba: [UInt8]) -> [UInt8] {
        [0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF] + rgba.flatMap { [$0, $0] }
    }

    /// The blocks in rows, `rowBytes` apart.
    public var bytes: [UInt8] {
        (0..<down).flatMap { y in (0..<across).flatMap { x in Self.voidExtent(Self.colour(x, y)) } }
    }

    /// The colour an LDR void-extent block decodes to (0...1); nil for any other block.
    public static func decode(_ block: ArraySlice<UInt8>) -> SIMD4<Float>? {
        let b = Array(block)
        guard b.count == blockBytes, b[0] == 0xFC, b[1] & 0x0F == 0x0D else { return nil }
        let unorm = (0..<4).map { Float(UInt16(b[8 + 2 * $0]) | UInt16(b[9 + 2 * $0]) << 8) / 65535 }
        return SIMD4(unorm[0], unorm[1], unorm[2], unorm[3])
    }

    /// The block whose colour `texel` is, by the CPU's decode of every block; nil when it is no block's.
    public func source(of texel: SIMD4<Float>) -> (x: Int, y: Int)? {
        guard (0..<4).allSatisfy({ texel[$0].isFinite && abs(texel[$0]) <= 2 }) else { return nil }
        let x = Int(((texel.x * 255 - 4) / 8).rounded()), y = Int(((texel.y * 255 - 4) / 8).rounded())
        guard x >= 0, x < across, y >= 0, y < down,
            let want = Self.decode(Self.voidExtent(Self.colour(x, y))[...]),
            (0..<4).allSatisfy({ abs(texel[$0] - want[$0]) < 1.0 / 512 })
        else { return nil }
        return (x, y)
    }
}

/// Where a block-compressed image's blocks lie in the GM20B's block-linear layout (TegraGM20BLayout.h
/// tgm_layout_offset_of: TegraColor GOBs, blocks one GOB wide and 2^yLog2 GOBs tall), and which address bits a
/// misplaced picture swapped.
public enum BlockLinearMap {
    /// The block height NIL picks for an image `texelHeight` tall with `rows` rows of blocks (raw/fx blockChainLayout).
    public static func yLog2(texelHeight: Int, rows: Int) -> Int {
        let byHeight =
            texelHeight >= 86 ? 4 : texelHeight >= 43 ? 3 : texelHeight >= 22 ? 2 : texelHeight >= 11 ? 1 : 0
        let gobRows = (rows + 7) / 8
        var clamp = 0
        while (1 << clamp) < gobRows { clamp += 1 }
        return min(byHeight, clamp)
    }

    public static func offset(xB: Int, y: Int, rowBytes: Int, yLog2: Int) -> Int {
        let blockRows = 8 << yLog2
        let block = (y / blockRows) * ((rowBytes + 63) / 64) + xB / 64
        let gob = ((xB % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xB % 32) / 16) * 32 + (y % 2) * 16 + xB % 16
        return block * (512 << yLog2) + ((y % blockRows) / 8) * 512 + gob
    }

    /// For each address bit in `bits` on which some block's displayed address (where the reader looked) and written
    /// address (where the writer put what it found) disagree, the XOR of written bits and constant that give it, as
    /// "g8=w11^w12,g11=w8^w11^w12^1"; "identity" when none disagrees, "gK=?" where no XOR of up to four bits fits.
    public static func bitMap(_ pairs: [(displayed: Int, written: Int)], bits: ClosedRange<Int>) -> String {
        var terms: [String] = []
        for k in bits where pairs.contains(where: { ($0.displayed >> k) & 1 != ($0.written >> k) & 1 }) {
            terms.append("g\(k)=" + (fit(k, pairs, bits: Array(bits)) ?? "?"))
        }
        return terms.isEmpty ? "identity" : terms.joined(separator: ",")
    }

    static func fit(_ k: Int, _ pairs: [(displayed: Int, written: Int)], bits: [Int]) -> String? {
        for size in 0...4 {
            for subset in combinations(bits, size) {
                for constant in 0...1
                where pairs.allSatisfy({ pair in
                    subset.reduce(constant) { $0 ^ (pair.written >> $1) & 1 } == (pair.displayed >> k) & 1
                }) {
                    let names = subset.map { "w\($0)" } + (constant == 1 ? ["1"] : [])
                    return names.isEmpty ? "0" : names.joined(separator: "^")
                }
            }
        }
        return nil
    }

    static func combinations(_ items: [Int], _ size: Int) -> [[Int]] {
        guard size > 0 else { return [[]] }
        guard items.count >= size else { return [] }
        let rest = Array(items.dropFirst())
        return combinations(rest, size - 1).map { [items[0]] + $0 } + combinations(rest, size)
    }
}
