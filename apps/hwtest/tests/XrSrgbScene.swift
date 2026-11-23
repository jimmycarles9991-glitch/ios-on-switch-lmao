import Foundation
import Metal

/// What gpu.gm20b.xr_srgb and gpu.gm20b.xr_srgb_alpha draw and what Metal makes of it. BGRA10_XR_sRGB (553) holds each
/// channel's 10-bit code in the top of a 16-bit word, BGR10_XR_sRGB (555) three codes in a 32-bit word (blue in bits
/// 0-9, red 20-29, two bits above). A colour code reads as the sRGB curve of (code - 384) / 510, mirrored below zero; an
/// alpha code as (code - 384) / 510 clamped to [0, 1].
struct XrSrgbFormat: Sendable {
    let format: MTLPixelFormat
    let bytes: Int
    let alpha: Bool

    static let bgra = XrSrgbFormat(format: .bgra10_xr_srgb, bytes: 8, alpha: true)
    static let bgr = XrSrgbFormat(format: .bgr10_xr_srgb, bytes: 4, alpha: false)

    var channels: Int { alpha ? 4 : 3 }

    /// Each texel's codes, red first
    func codes(_ bytes: [UInt8]) -> [[Int]] {
        stride(from: 0, to: bytes.count - self.bytes + 1, by: self.bytes).map { at in
            if alpha {
                let word = { (k: Int) in Int(bytes[at + 2 * k]) | Int(bytes[at + 2 * k + 1]) << 8 }
                return [word(2) >> 6, word(1) >> 6, word(0) >> 6, word(3) >> 6]
            }
            let word = Int(bytes[at]) | Int(bytes[at + 1]) << 8 | Int(bytes[at + 2]) << 16 | Int(bytes[at + 3]) << 24
            return [word >> 20 & 1023, word >> 10 & 1023, word & 1023]
        }
    }

    /// One texel's bytes from its codes, red first (555: the two bits above the codes `pad`)
    func texel(_ codes: [Int], pad: Int = 3) -> [UInt8] {
        if alpha {
            return [codes[2], codes[1], codes[0], codes[3]].flatMap { [UInt8(($0 << 6) & 0xff), UInt8($0 >> 2)] }
        }
        let word = UInt32(pad) << 30 | UInt32(codes[0]) << 20 | UInt32(codes[1]) << 10 | UInt32(codes[2])
        return (0 ..< 4).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) }
    }
}

enum XrSrgb {
    static let side = 8

    /// The value Metal reads for a code. An M4 Max's own reads lie within 6.1e-5 of it, nearest zero coarser
    /// (code 385 reads 2^-13), so a read is judged within that plus one step of a half, which the driver's
    /// RGBA16Float stand-in holds
    static func decoded(_ code: Int, alpha: Bool) -> Float {
        let e = (Double(code) - 384) / 510
        if alpha { return Float(min(max(e, 0), 1)) }
        let curve = { (x: Double) in x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4) }
        return Float(e < 0 ? -curve(-e) : curve(e))
    }

    static func tolerance(_ value: Float) -> Float {
        let half = Float(Float16(value)).magnitude
        let step = half < 0x1p-14 ? Float(0x1p-24) : Float(sign: .plus, exponent: half.exponent - 10, significand: 1)
        return 6.2e-5 + step
    }

    /// An unblended draw's values, one a texel: past both ends of the range, through the curve's linear foot, and
    /// alpha past [0, 1]; texel 0 is 0.5 in every channel
    static let drawn: [SIMD4<Float>] = (0 ..< 64).map { i in
        if i == 0 { return SIMD4(repeating: 0.5) }
        let f = Float(i)
        return SIMD4(
            -0.6 + f * 2.2 / 63, 1.6 - f * 2.2 / 63, Float(i % 9) / 8 * 0.005, Float(i * 5 % 64) / 63 * 1.4 - 0.2
        )
    }

    /// The source-over draw's values over Metal's codes of `drawn`
    static let blended: [SIMD4<Float>] = (0 ..< 64).map { i in
        let f = Float(i)
        return SIMD4(Float(i * 13 % 64) / 63 * 1.4 - 0.2, Float(i % 7) / 6, 1.2 - f / 63, Float(i * 3 % 16) / 15)
    }

    static let clears: [MTLClearColor] = [
        MTLClearColor(red: 0.5, green: 0.5, blue: 0.5, alpha: 0.5),
        MTLClearColor(red: -0.25, green: 1.4, blue: 0.0031, alpha: 0.7),
        MTLClearColor(red: 1.66, green: -0.52, blue: 0.2, alpha: 1.2),
        MTLClearColor(red: 0.0001, green: 0.04, blue: 0.9, alpha: -0.1),
    ]

    /// Every code in every channel, a texel each, on a 32x32 texture (555: the two bits above them 0 to 3 in turn)
    static func everyCode(_ f: XrSrgbFormat) -> [UInt8] {
        (0 ..< 1024).flatMap { f.texel([Int](repeating: $0, count: 4), pad: $0 & 3) }
    }
}
