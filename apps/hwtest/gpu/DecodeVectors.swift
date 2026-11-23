import Foundation
import Metal

/// The driver's decode vectors (drivers/TegraGM20BMetal/test/decode_vectors.h, written by harness/decodevectors.mm
/// on an M4 Max): blocks and what Apple's GPU reads from them. A want holds, per channel, an 8-bit code (an sRGB
/// format's colour channels through Apple's sRGB table `srgb`, its alpha as code / 255), the 12-bit k of a linear ASTC
/// form (Apple reads k / 4095), or the half bits of an HDR form (0xffff where Apple read a NaN).
public enum DecodeVectors {
    public struct Texel: Sendable {
        public let x: Int, y: Int
        public let want: [UInt32]
    }

    public struct Block: Sendable {
        public let words: [UInt32]
        public let texels: [Texel]
    }

    public struct Astc: Sendable {
        public let format: Int, blockW: Int, blockH: Int
        public let blocks: [Block]

        public var pixelFormat: MTLPixelFormat { MTLPixelFormat(rawValue: UInt(format))! }
        public var width: Int { blocks.count * blockW }

        /// The blocks side by side in one row, `rotated` blocks to the left (the first ones last)
        public func bytes(rotated: Int = 0) -> [UInt8] {
            let order = blocks.indices.map { blocks[($0 + rotated) % blocks.count] }
            return DecodeVectors.bytes(order.flatMap { $0.words })
        }

        /// Each vector's texel in that row
        public var points: [Point] {
            var out: [Point] = []
            for (index, block) in blocks.enumerated() {
                for texel in block.texels {
                    let at = SIMD2(UInt32(index * blockW + texel.x), UInt32(texel.y))
                    out.append(Point(block: index, texel: texel, at: at))
                }
            }
            return out
        }
    }

    public struct Point: Sendable {
        public let block: Int
        public let texel: Texel
        public let at: SIMD2<UInt32>
    }

    public struct Pvrtc: Sendable {
        public let format: Int, side: Int
        /// Morton-ordered, as replaceRegion takes a level (bytesPerRow 0)
        public let blocks: [UInt32]
        /// Each texel's RGBA8 codes, red in the low byte
        public let want: [UInt32]

        public var pixelFormat: MTLPixelFormat { MTLPixelFormat(rawValue: UInt(format))! }
        public var srgb: Bool { format & 1 == 1 }
        public var bytes: [UInt8] { DecodeVectors.bytes(blocks) }
    }

    public static let astc: [Int: Astc] = Dictionary(
        uniqueKeysWithValues: (ldrText + hdrText).map { entry -> (Int, Astc) in
            let (format, blockW, blockH, text) = entry
            let blocks = text.split(separator: "\n").compactMap { line -> Block? in
                let w = words(String(line))
                guard w.count == 16 else { return nil }
                return Block(
                    words: Array(w[0 ..< 4]),
                    texels: [
                        Texel(x: Int(w[4]), y: Int(w[5]), want: Array(w[6 ..< 10])),
                        Texel(x: Int(w[10]), y: Int(w[11]), want: Array(w[12 ..< 16])),
                    ]
                )
            }
            return (format, Astc(format: format, blockW: blockW, blockH: blockH, blocks: blocks))
        }
    )

    /// A format's vectors; none (which makes no texture: a test's FAIL, not a trap) where the table lacks them
    public static func set(_ format: Int) -> Astc {
        astc[format] ?? Astc(format: format, blockW: 1, blockH: 1, blocks: [])
    }

    /// The level of `format` and `side`
    public static func level(_ format: Int, side: Int) -> Pvrtc? {
        pvrtc.first { $0.format == format && $0.side == side }
    }

    public static let pvrtc: [Pvrtc] = pvrtcText.map { entry -> Pvrtc in
        let (format, side, text) = entry
        let w = words(text.split(separator: "\n").joined())
        let texels = min(side * side, w.count)
        return Pvrtc(
            format: format, side: side, blocks: Array(w[0 ..< (w.count - texels)]), want: Array(w.suffix(texels))
        )
    }

    /// Little-endian words as bytes
    public static func bytes(_ words: [UInt32]) -> [UInt8] {
        words.flatMap { word in (0 ..< 4).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) } }
    }

    /// 256 floats; a table that did not decode reads as zeros, which no sRGB judge passes
    public static let srgb: [Float] = {
        let table = words(srgbText.split(separator: "\n").joined()).map(Float.init(bitPattern:))
        return table.count == 256 ? table : [Float](repeating: 0, count: 256)
    }()

    static func words(_ base64: String) -> [UInt32] {
        let data = Data(base64Encoded: base64) ?? Data()
        return stride(from: 0, to: data.count, by: 4).map { at in
            (0 ..< 4).reduce(UInt32(0)) { $0 | UInt32(data[data.startIndex + at + $1]) << (8 * $1) }
        }
    }

    /// The nearest code of Apple's sRGB table to `value`
    public static func nearestSrgbCode(_ value: Float) -> Int {
        var best = 0
        for code in 1 ..< srgb.count where abs(srgb[code] - value) < abs(srgb[best] - value) { best = code }
        return best
    }

    /// The driver's 16-bit code of a linear ASTC form's 12-bit k (decoded.h kRgba16Unorm stored: the nearest
    /// 16-bit code of k / 4095)
    public static func unorm16(_ k: UInt32) -> UInt32 { 16 * k + (k + 136) / 273 }

    /// A half's bits as a float; nil for a NaN (any NaN matches)
    public static func half(_ bits: UInt32) -> Float? {
        let value = Float(Float16(bitPattern: UInt16(truncatingIfNeeded: bits)))
        return value.isNaN ? nil : value
    }
}
