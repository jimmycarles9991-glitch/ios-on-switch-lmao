import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The decode tests' comparisons of read() floats with DecodeVectors' wants: each texel whose codes differ counted
/// once, and the largest float distance of a measured form, for reports.
enum DecodeJudge {
    struct Tally {
        var off = 0
        var maxDiff: Float = 0
        /// Of an sRGB read's colour channels off Apple's code (channelsOff): those read as the code / 255, no sRGB
        /// decode where the texel is read (undecoded), and those read as the decode of the code's own decode stored in
        /// 8 bits, one decode too many (twice)
        var channelsOff = 0, undecoded = 0, twice = 0

        /// "undecoded/twice/channels off", for a report
        var forms: String { "\(undecoded)/\(twice)/\(channelsOff)" }

        mutating func add(_ other: Tally) {
            off += other.off
            maxDiff = max(maxDiff, other.maxDiff)
            channelsOff += other.channelsOff
            undecoded += other.undecoded
            twice += other.twice
        }
    }

    /// 8-bit codes, red in the low byte of `want`; an sRGB read's colour channels by the nearest code of Apple's table
    /// (DecodeVectors.srgb), its distance from that code's value in `maxDiff`; alpha always code / 255
    static func rgba8(_ read: [SIMD4<Float>], _ want: [UInt32], srgb: Bool) -> Tally {
        var tally = Tally()
        for (texel, word) in zip(read, want) {
            var off = false
            for c in 0 ..< 4 {
                let code = Int((word >> (8 * UInt32(c))) & 255)
                if srgb && c < 3 {
                    let channelOff = DecodeVectors.nearestSrgbCode(texel[c]) != code
                    off = off || channelOff
                    tally.maxDiff = max(tally.maxDiff, abs(texel[c] - DecodeVectors.srgb[code]))
                    if channelOff {
                        let twice = DecodeVectors.srgb[Self.code(DecodeVectors.srgb[code], 255)]
                        tally.channelsOff += 1
                        tally.undecoded += abs(texel[c] - Float(code) / 255) < 0.5 / 255 ? 1 : 0
                        tally.twice += abs(texel[c] - twice) < 0.5 / 255 ? 1 : 0
                    }
                } else {
                    off = off || Self.code(texel[c], 255) != code
                    tally.maxDiff = max(tally.maxDiff, abs(texel[c] - Float(code) / 255))
                }
            }
            if off { tally.off += 1 }
        }
        return tally
    }

    /// `value`'s nearest code of a `scale`-code unorm; -1 for a value no code holds (not finite, or outside 0...1)
    static func code(_ value: Float, _ scale: Float) -> Int {
        value.isFinite && value >= 0 && value <= 1 ? Int((value * scale).rounded()) : -1
    }

    /// A vector's want as one 8-bit word, red low (sRGB and alpha codes of the narrow and native sRGB ASTC forms)
    static func word(_ want: [UInt32]) -> UInt32 {
        want.enumerated().reduce(UInt32(0)) { $0 | ($1.element & 255) << (8 * UInt32($1.offset)) }
    }

    struct Linear {
        /// Texels whose 16-bit code is not the driver's rule of Apple's 12-bit k (DecodeVectors.unorm16)
        var off = 0
        /// The largest distance from Apple's k / 4095, in 16-bit codes
        var appleMaxCodes = 0.0
        /// Texels an 8-bit target would hold as another code than Apple's value rounds to
        var eightBitOff = 0
    }

    /// A linear 4x2 or 8x4 form held in 16 bits: each channel's code against the driver's rule of `want`'s k
    static func unorm16(_ read: [SIMD4<Float>], _ wants: [[UInt32]]) -> Linear {
        var linear = Linear()
        for (texel, want) in zip(read, wants) {
            var off = false, eight = false
            for c in 0 ..< 4 {
                let value = Double(texel[c]), k = Double(want[c])
                off = off || Self.code(texel[c], 65535) != Int(DecodeVectors.unorm16(want[c]))
                linear.appleMaxCodes = max(linear.appleMaxCodes, abs(value * 65535 - k * 65535 / 4095))
                eight = eight || (value * 255).rounded() != (k / 4095 * 255).rounded()
            }
            if off { linear.off += 1 }
            if eight { linear.eightBitOff += 1 }
        }
        return linear
    }

    struct Halves {
        var off = 0
        /// Channels where Apple read a NaN and this read none, or the other way round
        var nanMismatch = 0
    }

    /// Half bits: every channel bit for bit as a float, a NaN where Apple read one (any NaN)
    static func halves(_ read: [SIMD4<Float>], _ wants: [[UInt32]]) -> Halves {
        var halves = Halves()
        for (texel, want) in zip(read, wants) {
            var off = false
            for c in 0 ..< 4 {
                if let value = DecodeVectors.half(want[c]) {
                    if texel[c].isNaN { halves.nanMismatch += 1 }
                    off = off || texel[c].bitPattern != value.bitPattern
                } else if !texel[c].isNaN {
                    halves.nanMismatch += 1
                    off = true
                }
            }
            if off { halves.off += 1 }
        }
        return halves
    }

    /// Texels that are not a NaN in every channel (a never-written texel of an HDR form)
    static func notAllNaN(_ read: [SIMD4<Float>]) -> Int {
        read.filter { texel in (0 ..< 4).contains { !texel[$0].isNaN } }.count
    }
}
