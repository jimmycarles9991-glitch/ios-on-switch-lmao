import Foundation

/// An M4 Max's RGB9E5Float mip step (the mip rule package's model3d.h and rgb9e5_model.h, 0 texels off Apple's): the
/// texture unit's linear sample at the texel's centre with weights in units of 2^-16, x times z exact and the second
/// row's share of it (W qy + 127) >> 8; a plane's four terms summed in block float, 10 fraction bits and 7 guard bits
/// under the largest term's top bit, each term truncated; rounded to a half, half away from zero; stored truncating.
enum Rgb9e5MipModel {
    static func msb(_ v: UInt64) -> Int { 63 - v.leadingZeroBitCount }

    static func shifted(_ p: UInt64, _ by: Int) -> UInt64 {
        by >= 0 ? (by < 64 ? p << UInt64(by) : 0) : (-by < 64 ? p >> UInt64(-by) : 0)
    }

    /// Texel (x, y) of a 2D level from its taps (x0 y0, x1 y0, x0 y1, x1 y1) at weights qx, qy of 256
    static func step(_ taps: [UInt32], qx: UInt32, qy: UInt32) -> UInt32 {
        var n = [UInt64](repeating: 0, count: 4)
        for k in 0 ..< 2 {
            let xz = UInt64(k == 1 ? qx : 256 - qx) * 256
            let y1 = (xz * UInt64(qy) + 127) >> 8
            n[k | 2] = y1
            n[k] = xz - y1
        }
        let halves = (0 ..< 3).map { c -> UInt32 in
            let m = taps.map { UInt64($0 >> UInt32(9 * c) & 511) }
            let z = taps.map { Int($0 >> 27 & 31) - 24 }
            return half(sum(m, z, n))
        }
        return word(halves)
    }

    /// The terms m 2^z n 2^-16 aligned to the largest's estimated top bit, 7 guard bits under a half's 10, each truncated
    static func sum(_ m: [UInt64], _ z: [Int], _ n: [UInt64]) -> (s: UInt64, sc: Int) {
        let live = (0 ..< m.count).filter { m[$0] != 0 && n[$0] != 0 }
        guard let e = live.map({ msb(m[$0]) + z[$0] + msb(n[$0]) - 16 }).max() else { return (0, 0) }
        let sc = e - 17
        return (live.reduce(0) { $0 + shifted(m[$1] * n[$1], z[$1] - 16 - sc) }, sc)
    }

    /// s 2^sc as a half's bits, half away from zero, subnormals kept, overflow to infinity
    static func half(_ v: (s: UInt64, sc: Int)) -> UInt32 {
        guard v.s != 0 else { return 0 }
        var ulp = max(msb(v.s) + v.sc, -14) - 10
        let drop = ulp - v.sc
        var r =
            drop > 0 ? shifted(v.s, -drop) + (drop <= 64 ? shifted(v.s, 1 - drop) & 1 : 0) : shifted(v.s, -drop)
        if r == 2048 {
            r >>= 1
            ulp += 1
        }
        guard r != 0 else { return 0 }
        let x = msb(r) + ulp
        if x > 15 { return 0x7c00 }
        if x < -14 { return UInt32(r) }
        return UInt32(x + 15) << 10 | UInt32(r) & 0x3ff
    }

    /// Three halves as an RGB9E5 word: the largest's exponent shared, every mantissa truncated
    static func word(_ h: [UInt32]) -> UInt32 {
        let mx = h.max() ?? 0
        guard mx != 0 else { return 0 }
        let parts = { (v: UInt32) -> (m: UInt32, z: Int) in
            let e = v >> 10
            return e != 0 ? (v & 0x3ff | 0x400, Int(e) - 25) : (v & 0x3ff, -24)
        }
        let top = parts(mx)
        let es = min(max(msb(UInt64(top.m)) + top.z, -16) + 16, 31)
        return h.enumerated().reduce(UInt32(es) << 27) { word, item in
            let (m, z) = parts(item.element)
            let by = z - es + 24
            let v = by >= 0 ? m << UInt32(by) : (-by < 32 ? m >> UInt32(-by) : 0)
            return word | min(v, 511) << UInt32(9 * item.offset)
        }
    }
}
