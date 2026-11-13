import Foundation

/// Metal's store of a render pass into a two-plane YCbCr attachment over plane 0 of a CoreVideo surface, as
/// drivers/TegraGM20BMetal/raw/runtime/raw_ycbcr_store_rule.h states it (an M4 Max's bytes): code formats (540, 541,
/// 543, 544) hold each texel as a half and store each texel's green code and each chroma block's rounded mean code; the
/// converted ones (500-507) are m2v's rule over the same halves, and their sRGB twins (520-527) the same rule over the
/// halves' sRGB steps.
public enum YcbcrStore {
    public enum Source: Sendable {
        case drawnFloat, drawnHalf, clear
    }

    static func held(_ v: Float, towardZero: Bool) -> Double {
        let step = Double(sign: .plus, exponent: max(Int(v.exponent), -14) - 10, significand: 1)
        let q = Double(v) / step
        return (towardZero ? q.rounded(.down) : q.rounded(.toNearestOrEven)) * step
    }

    /// A held channel's code of `max` (255 or 1023): 10-bit codes take a drawn float toward zero.
    public static func code(_ v: Float, _ source: Source, max: UInt32) -> UInt32 {
        if v.isNaN || v <= 0 { return 0 }
        if v >= 1 { return max }
        let h = source == .drawnHalf ? Double(v) : held(v, towardZero: max == 1023 && source == .drawnFloat)
        return UInt32((h * Double(max) + 0.5).rounded(.down))
    }

    /// Luma codes, then Cb and Cr of each chroma block (2 x 2 texels, 2 x 1 where not `halvedDown`, 1 x 1 where not
    /// `halvedAcross` either), row by row.
    public static func codes(
        _ texels: [SIMD4<Float>], width: Int, height: Int, max: UInt32, halvedAcross: Bool = true, halvedDown: Bool,
        source: Source
    ) -> (luma: [UInt16], chroma: [UInt16]) {
        let luma = texels.map { UInt16(code($0.y, source, max: max)) }
        let across = halvedAcross ? 2 : 1, down = halvedDown ? 2 : 1, n = UInt32(across * down)
        var chroma: [UInt16] = []
        for y in 0..<(height / down) {
            for x in 0..<(width / across) {
                for channel in [2, 0] {
                    var sum: UInt32 = 0
                    for dy in 0..<down {
                        for dx in 0..<across {
                            sum += code(texels[(y * down + dy) * width + across * x + dx][channel], source, max: max)
                        }
                    }
                    chroma.append(UInt16((sum + n / 2) / n))
                }
            }
        }
        return (luma, chroma)
    }

    /// A channel as Metal holds it, clamped to [0, 1], a NaN 0: 10-bit targets take a drawn float toward zero.
    static func held(_ v: Float, _ source: Source, bits: Int) -> Double {
        if v.isNaN || v <= 0 { return 0 }
        if v >= 1 { return 1 }
        if source == .drawnHalf { return Double(v) }
        return held(v, towardZero: bits == 10 && source == .drawnFloat)
    }

    /// A converted (YCBCR*_2P) or sRGB (YCBCR*_2P_sRGB) target over a surface without a matrix (BT.601), video or full
    /// range: luma codes, then Cb and Cr of each chroma block.
    public static func converted(
        _ texels: [SIMD4<Float>], width: Int, height: Int, bits: Int = 8, full: Bool, halvedAcross: Bool = true,
        halvedDown: Bool, srgb: Bool = false, source: Source = .drawnFloat
    ) -> (luma: [UInt16], chroma: [UInt16]) {
        let q15: [[Int64]] = [[9797, 19235, 3736], [-5537, -10846, 16383], [16384, -13730, -2654]]
        let video: [([Int64], Int64)] = [
            ([8381, 16455, 3196], 18), ([-4845, -9491, 14336], 18), ([7168, -6007, -1161], 17),
        ]
        let ten: Int64 = bits == 10 ? 2 : 0
        let rows: [[Int64]] = (0..<3).map { code in
            full
                ? q15[code].map { $0 * (code == 0 ? (1 << bits) - 1 : 1 << bits) }
                : video[code].0.map { $0 << (26 - video[code].1 + ten) }
        }
        let offsets: [Int64] = [full ? 0 : 16 << (bits - 8), 1 << (bits - 1), 1 << (bits - 1)]
        let max = Int64(1 << bits) - 1, termBits = Int64(bits == 10 ? 4 : 6)
        let shift = Int64(srgb ? 22 : 20) + Int64(bits - 8)
        let step = { (v: Float) -> Int64 in
            let h = held(v, source, bits: bits)
            return srgb
                ? Int64(YcbcrSrgb.encode[Int((h * 8192).rounded(.toNearestOrEven))])
                : Int64((h * 2048).rounded(.toNearestOrEven))
        }
        let steps = texels.map { [step($0.x), step($0.y), step($0.z)] }
        let term = { (code: Int, texel: Int) -> Int64 in
            (0..<3).reduce(Int64(0)) { $0 + rows[code][$1] * steps[texel][$1] } >> shift
        }
        let encode = { (code: Int, sum: Int64, k: Int64) -> UInt16 in
            UInt16(Swift.min(Swift.max(offsets[code] + ((sum + (1 << (termBits - 1 + k))) >> (termBits + k)), 0), max))
        }
        let luma = (0..<texels.count).map { encode(0, term(0, $0), 0) }
        let across = halvedAcross ? 2 : 1, down = halvedDown ? 2 : 1
        let k = Int64((halvedAcross ? 1 : 0) + (halvedDown ? 1 : 0))
        var chroma: [UInt16] = []
        for y in 0..<(height / down) {
            for x in 0..<(width / across) {
                for code in 1...2 {
                    var sum: Int64 = 0
                    for dy in 0..<down {
                        for dx in 0..<across { sum += term(code, (y * down + dy) * width + across * x + dx) }
                    }
                    chroma.append(encode(code, sum, k))
                }
            }
        }
        return (luma, chroma)
    }
}
