import Foundation

/// How far apart two results of one operation are: per texel, the largest difference of any channel's stored integer
/// (a unorm code, or a float's bit pattern, which counts ulps between floats of one sign), counted over the texels.
public enum BenchDifference {
    /// Each format's channels as stored, from the texel's lowest bit up
    public static let channelBits: [String: [Int]] = [
        "r8unorm": [8], "rg8unorm": [8, 8], "rgba8unorm": [8, 8, 8, 8], "bgra8unorm": [8, 8, 8, 8],
        "rgba8unorm_srgb": [8, 8, 8, 8], "bgra8unorm_srgb": [8, 8, 8, 8], "r16unorm": [16], "rg16unorm": [16, 16],
        "rgba16unorm": [16, 16, 16, 16], "r16float": [16], "rg16float": [16, 16], "rgba16float": [16, 16, 16, 16],
        "r32float": [32], "rg32float": [32, 32], "rgba32float": [32, 32, 32, 32], "rgb10a2unorm": [10, 10, 10, 2],
        "bgr10a2unorm": [10, 10, 10, 2], "b5g6r5unorm": [5, 6, 5], "bgr5a1unorm": [5, 5, 5, 1],
        "rg11b10float": [11, 11, 10],
    ]
    /// Differences above this are counted together
    public static let largest = 64

    /// `{"0": texels the same, "1": texels one apart at most, ..., ">64": the rest}`; empty where the sizes differ
    public static func histogram(_ a: Data, _ b: Data, format: String, texelBytes: Int) -> [String: Int] {
        guard a.count == b.count, texelBytes > 0, let bits = channelBits[format] else { return [:] }
        var counts: [String: Int] = [:]
        let x = [UInt8](a), y = [UInt8](b)
        for start in stride(from: 0, to: x.count - texelBytes + 1, by: texelBytes) {
            var most = 0, at = 0
            for width in bits {
                let d = abs(field(x, start, at, width) - field(y, start, at, width))
                most = max(most, d)
                at += width
            }
            counts[most > largest ? ">\(largest)" : String(most), default: 0] += 1
        }
        return counts
    }

    /// Bits [at, at + width) of the little-endian texel at byte `start`
    static func field(_ bytes: [UInt8], _ start: Int, _ at: Int, _ width: Int) -> Int {
        var v = 0
        for bit in 0..<width {
            let b = at + bit
            v |= Int(bytes[start + b / 8] >> UInt8(b % 8) & 1) << bit
        }
        return v
    }
}
