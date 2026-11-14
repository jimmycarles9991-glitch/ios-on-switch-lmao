import Foundation

/// The 2D engine's 2:1 box of two unorm8 samples as the chip reads it (raw_resolve_rule.h): each code c read as
/// round(c * 2^15 / 255), then the ROP's store rule of the exact mean (cut to 12 fractional bits, ties down).
public enum X1TwodResolveReading {
    public static func code15(_ c: UInt32) -> UInt32 { (257 * c + 1) >> 1 }

    public static func box(_ a: UInt32, _ b: UInt32) -> UInt32 {
        let u = (code15(a) + code15(b)) >> 4
        return (u * 255 + 2047) >> 12
    }

    /// The box of the complemented samples read under ROP3 NOTSRCCOPY
    public static func complement(_ a: UInt32, _ b: UInt32) -> UInt32 { 255 - box(255 - a, 255 - b) }
}
