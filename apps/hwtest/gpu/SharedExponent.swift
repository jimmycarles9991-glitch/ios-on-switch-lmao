import Foundation

/// RGB9E5 (MTLPixelFormat.rgb9e5Float): three 9-bit mantissas under one 5-bit exponent, bias 15, no sign. A value
/// whose largest mantissa is below 256 has several encodings; MSL 8.7.5 encodes it under the smallest exponent its
/// mantissas fit under, and Apple's GPUs keep the format by value, so a blit of any other encoding reads back that one.
public enum SharedExponent {
    public static func encoding(_ word: UInt32) -> UInt32 {
        var exponent = word >> 27
        var mantissas = [word & 0x1FF, word >> 9 & 0x1FF, word >> 18 & 0x1FF]
        while exponent > 0 && mantissas.max()! < 256 {
            mantissas = mantissas.map { $0 << 1 }
            exponent -= 1
        }
        return mantissas[0] | mantissas[1] << 9 | mantissas[2] << 18 | exponent << 27
    }

    /// Each little-endian word of `bytes` as its `encoding`.
    public static func encodings(_ bytes: [UInt8]) -> [UInt8] {
        stride(from: 0, to: bytes.count, by: 4).flatMap { at -> [UInt8] in
            let word = (0..<4).reduce(UInt32(0)) { $0 | UInt32(bytes[at + $1]) << (8 * $1) }
            return withUnsafeBytes(of: encoding(word).littleEndian, Array.init)
        }
    }
}
