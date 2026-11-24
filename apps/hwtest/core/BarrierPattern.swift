import Foundation

/// What the barrier probes' pass A stores at (x, y) for a seed (X1Probes.metal x1_seeded_*), in memory order: the
/// hash's bytes as BGRA8 codes, ten bits of it a component as halves in [1, 2), or 23 bits a component (each three bits
/// on from the last) as floats in [1, 2).
public enum BarrierPattern: Sendable {
    case bgra8, rgba16f, rgba32f

    public var bytesPerTexel: Int {
        switch self {
        case .bgra8: return 4
        case .rgba16f: return 8
        case .rgba32f: return 16
        }
    }

    public func texel(_ x: Int, _ y: Int, seed: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: bytesPerTexel)
        out.withUnsafeMutableBytes { put(x, y, seed: seed, into: $0, at: 0) }
        return out
    }

    /// The pattern over width x height, rows packed.
    public func image(width: Int, height: Int, seed: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: width * height * bytesPerTexel)
        out.withUnsafeMutableBytes { bytes in
            for y in 0 ..< height {
                for x in 0 ..< width { put(x, y, seed: seed, into: bytes, at: (y * width + x) * bytesPerTexel) }
            }
        }
        return out
    }

    @inline(__always)
    public func put(_ x: Int, _ y: Int, seed: Int, into out: UnsafeMutableRawBufferPointer, at offset: Int) {
        let v = probeHash(UInt32(x), UInt32(y), UInt32(seed))
        switch self {
        case .bgra8:
            let codes = (v >> 16) & 255 | (v & 0xff00) | (v & 255) << 16 | v & 0xff00_0000
            out.storeBytes(of: codes.littleEndian, toByteOffset: offset, as: UInt32.self)
        case .rgba16f:
            store(UInt16(0x3c00 | v & 0x3ff), out, offset)
            store(UInt16(0x3c00 | (v >> 10) & 0x3ff), out, offset + 2)
            store(UInt16(0x3c00 | (v >> 20) & 0x3ff), out, offset + 4)
            store(UInt16(0x3c00 | (v >> 22) & 0x3ff), out, offset + 6)
        case .rgba32f:
            for i in 0 ..< 4 {
                let bits = 0x3f80_0000 | (v >> (3 * i)) & 0x7f_ffff
                out.storeBytes(of: bits.littleEndian, toByteOffset: offset + i * 4, as: UInt32.self)
            }
        }
    }

    @inline(__always)
    private func store(_ half: UInt16, _ out: UnsafeMutableRawBufferPointer, _ offset: Int) {
        out.storeBytes(of: half.littleEndian, toByteOffset: offset, as: UInt16.self)
    }
}

/// The first byte at which `got` and `want` differ (the shorter one's end when one is the other's start), nil when
/// they are equal.
public func firstDifference(_ got: [UInt8], _ want: [UInt8]) -> Int? {
    let count = min(got.count, want.count)
    let same = got.withUnsafeBytes { a in
        want.withUnsafeBytes { b in count == 0 || memcmp(a.baseAddress!, b.baseAddress!, count) == 0 }
    }
    if same { return got.count == want.count ? nil : count }
    return (0 ..< count).first { got[$0] != want[$0] }
}
