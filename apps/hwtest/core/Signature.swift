import Foundation

/// A rendered attachment in a form two GPUs can be compared by: the FNV-1a hash of its bytes, and the mean of every
/// channel over each 8x8 block, decoded from the attachment's format. Another GPU's filtering and half-precision
/// rounding move a block's mean by a step or two; a wrong blend factor, format, swizzle or missing texture moves it
/// by far more. A block with a NaN or an infinity in it has a NaN mean, which only another NaN matches.
public struct Signature: Equatable, Sendable {
    public static let block = 8

    public let hash: UInt64
    public let means: [Float]

    /// QuartzCore's private two-plane extended-range format: a BGR10_XR word in plane 0 and alpha in plane 1.
    public static let twoPlaneXR: UInt = 550

    /// The texel format of a Metal pixel format number among those Core Animation renders to (550's plane 0 as
    /// BGR10_XR); nil for the rest, which compare by hash alone.
    public static func format(metal: UInt) -> TexelFormat? {
        let formats: [UInt: TexelFormat] = [
            1: .a8Unorm, 10: .r8Unorm, 30: .rg8Unorm, 70: .rgba8Unorm, 71: .rgba8Srgb, 80: .bgra8Unorm, 81: .bgra8Srgb,
            25: .r16Float, 65: .rg16Float, 115: .rgba16Float, 55: .r32Float, 125: .rgba32Float, 90: .rgb10a2Unorm,
            twoPlaneXR: .bgr10XR, 552: .bgra10XR, 553: .bgra10XR, 554: .bgr10XR, 555: .bgr10XR,
        ]
        return formats[metal]
    }

    /// The bytes a blit readback of `metalFormat` defines. A 550 texture read into rows of 8 bytes a texel gets plane
    /// 0 alone, a 4-byte word a texel at the front of each row, as Apple's GPUs blit it; the rest of each row is
    /// whatever the buffer held.
    public static func blitted(_ bytes: [UInt8], metalFormat: UInt, width: Int, height: Int) -> [UInt8] {
        guard metalFormat == twoPlaneXR, bytes.count == width * height * 8 else { return bytes }
        return (0..<height).flatMap { row in bytes[(row * width * 8)..<(row * width * 8 + width * 4)] }
    }

    /// Whether the bytes of each row past 550's plane 0 still hold `sentinel`, the buffer's fill before the blit.
    public static func padUntouched(
        _ bytes: [UInt8], metalFormat: UInt, width: Int, height: Int, sentinel: UInt8
    ) -> Bool {
        guard metalFormat == twoPlaneXR, bytes.count == width * height * 8 else { return true }
        return (0..<height).allSatisfy { row in
            bytes[(row * width * 8 + width * 4)..<((row + 1) * width * 8)].allSatisfy { $0 == sentinel }
        }
    }

    public init(hash: UInt64, means: [Float]) {
        self.hash = hash
        self.means = means
    }

    public init(bytes readBack: [UInt8], metalFormat: UInt, width: Int, height: Int) {
        let bytes = Self.blitted(readBack, metalFormat: metalFormat, width: width, height: height)
        hash = Self.fnv1a(bytes)
        guard let format = Self.format(metal: metalFormat), bytes.count == width * height * format.bytesPerTexel else {
            means = []
            return
        }
        let channels = format.channels
        let (columns, rows) = ((width + Self.block - 1) / Self.block, (height + Self.block - 1) / Self.block)
        var sums = [Float](repeating: 0, count: columns * rows * channels)
        var counts = [Float](repeating: 0, count: columns * rows)
        for y in 0..<height {
            for x in 0..<width {
                let at = (y * width + x) * format.bytesPerTexel
                let values = format.decode(bytes[at..<(at + format.bytesPerTexel)])
                let cell = (y / Self.block) * columns + x / Self.block
                counts[cell] += 1
                for c in 0..<channels {
                    sums[cell * channels + c] += values[c].isFinite ? values[c] : .nan
                }
            }
        }
        means = sums.indices.map { sums[$0] / counts[$0 / channels] }
    }

    /// The same bytes, or every block mean within `absolute` plus `relative` of its magnitude (NaN only matches NaN).
    public func matches(_ other: Signature, absolute: Float, relative: Float) -> Bool {
        if hash == other.hash { return true }
        guard !means.isEmpty, means.count == other.means.count else { return false }
        return zip(means, other.means).allSatisfy { a, b in
            if a.isNaN || b.isNaN { return a.isNaN && b.isNaN }
            return abs(a - b) <= absolute + relative * max(abs(a), abs(b))
        }
    }

    /// "<hash in hex>[:<means>]", each mean as four hex digits of its half-precision bits: a mean is compared within
    /// a few steps of its format, which half precision holds.
    public var text: String {
        let hex = String(hash, radix: 16)
        guard !means.isEmpty else { return hex }
        return hex + ":" + means.map { String(format: "%04x", Float16($0).bitPattern) }.joined()
    }

    public init?(text: String) {
        let parts = text.split(separator: ":", maxSplits: 1).map(Array.init)
        guard let first = parts.first, let hash = UInt64(String(first), radix: 16) else { return nil }
        let digits = parts.count > 1 ? parts[1] : []
        guard digits.count % 4 == 0 else { return nil }
        let bits = stride(from: 0, to: digits.count, by: 4).map { UInt16(String(digits[$0..<($0 + 4)]), radix: 16) }
        guard bits.allSatisfy({ $0 != nil }) else { return nil }
        self.init(hash: hash, means: bits.map { Float(Float16(bitPattern: $0!)) })
    }

    public static func fnv1a(_ bytes: some Sequence<UInt8>) -> UInt64 {
        bytes.reduce(0xcbf2_9ce4_8422_2325) { ($0 ^ UInt64($1)) &* 0x100_0000_01b3 }
    }

    public static func == (lhs: Signature, rhs: Signature) -> Bool {
        lhs.hash == rhs.hash && lhs.means.count == rhs.means.count
            && zip(lhs.means, rhs.means).allSatisfy { $0 == $1 || ($0.isNaN && $1.isNaN) }
    }
}
