import Foundation
import Metal

/// A chain format's texel as codes: bit fields of a little-endian texel, each an unorm, an sRGB-encoded unorm or an
/// unsigned-range float of its width (16 half, 32 float, 11 and 10 the packed float's), compared code by code: one code
/// apart is one unorm step or one float ulp (every value here is non-negative, so float codes order as their values).
struct MipLayout {
    enum Kind { case unorm, srgb, float }

    struct Field {
        let shift: Int
        let width: Int
        let kind: Kind
        var mask: UInt32 { width == 32 ? .max : (1 << UInt32(width)) - 1 }
    }

    let bytes: Int
    let fields: [Field]

    static func unorm(_ widths: [Int], srgb: Bool = false) -> MipLayout {
        var shift = 0
        var fields: [Field] = []
        for (i, w) in widths.enumerated() {
            fields.append(Field(shift: shift, width: w, kind: srgb && i < 3 ? .srgb : .unorm))
            shift += w
        }
        return MipLayout(bytes: shift / 8, fields: fields)
    }

    static func floats(_ widths: [Int]) -> MipLayout {
        var shift = 0
        var fields: [Field] = []
        for w in widths {
            fields.append(Field(shift: shift, width: w, kind: .float))
            shift += w
        }
        return MipLayout(bytes: shift / 8, fields: fields)
    }

    func code(_ bytes: [UInt8], texel: Int, field f: Field) -> UInt32 {
        let at = texel * self.bytes
        if self.bytes <= 4 {
            var word: UInt32 = 0
            for b in 0..<self.bytes { word |= UInt32(bytes[at + b]) << UInt32(8 * b) }
            return (word >> UInt32(f.shift)) & f.mask
        }
        var value: UInt32 = 0
        for b in 0..<(f.width / 8) { value |= UInt32(bytes[at + f.shift / 8 + b]) << UInt32(8 * b) }
        return value
    }

    func put(_ code: UInt32, _ bytes: inout [UInt8], texel: Int, field f: Field) {
        let at = texel * self.bytes
        if self.bytes <= 4 {
            var word: UInt32 = 0
            for b in 0..<self.bytes { word |= UInt32(bytes[at + b]) << UInt32(8 * b) }
            word = (word & ~(f.mask << UInt32(f.shift))) | ((code & f.mask) << UInt32(f.shift))
            for b in 0..<self.bytes { bytes[at + b] = UInt8(truncatingIfNeeded: word >> UInt32(8 * b)) }
            return
        }
        for b in 0..<(f.width / 8) { bytes[at + f.shift / 8 + b] = UInt8(truncatingIfNeeded: code >> UInt32(8 * b)) }
    }

    /// The field's value: an unorm's fraction, an sRGB code's linear value, a float's value
    func value(_ code: UInt32, _ f: Field) -> Double {
        switch f.kind {
        case .unorm: return Double(code) / Double(f.mask)
        case .srgb:
            let c = Double(code) / 255
            return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)
        case .float:
            switch f.width {
            case 32: return Double(Float(bitPattern: code))
            case 16: return Double(MipLayout.half(code))
            default: return MipLayout.smallFloat(code, mantissa: f.width - 5)
            }
        }
    }

    /// The code of `value`, rounded to nearest
    func code(of value: Double, _ f: Field) -> UInt32 {
        switch f.kind {
        case .unorm: return UInt32(min(max(value, 0), 1) * Double(f.mask) + 0.5)
        case .srgb:
            let v = min(max(value, 0), 1)
            let c = v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055
            return UInt32(c * 255 + 0.5)
        case .float:
            switch f.width {
            case 32: return Float(value).bitPattern
            case 16: return MipLayout.halfBits(value)
            default: return MipLayout.smallFloatBits(value, mantissa: f.width - 5)
            }
        }
    }

    static func half(_ bits: UInt32) -> Double {
        smallFloat(bits & 0x7fff, mantissa: 10)
    }

    static func halfBits(_ value: Double) -> UInt32 {
        smallFloatBits(value, mantissa: 10)
    }

    /// An unsigned float of a 5-bit exponent (bias 15) and `mantissa` bits
    static func smallFloat(_ bits: UInt32, mantissa m: Int) -> Double {
        let e = Int(bits >> UInt32(m)) & 31, f = Double(bits & ((1 << UInt32(m)) - 1)) / Double(1 << m)
        return e == 0 ? f * pow(2, -14) : (1 + f) * pow(2, Double(e - 15))
    }

    static func smallFloatBits(_ value: Double, mantissa m: Int) -> UInt32 {
        guard value > 0 else { return 0 }
        let scale = Double(1 << m)
        var e = Int(floor(log2(value))) + 15
        if e <= 0 { return UInt32((value / pow(2, -14) * scale).rounded(.toNearestOrEven)) }
        var f = ((value / pow(2, Double(e - 15)) - 1) * scale).rounded(.toNearestOrEven)
        if f >= scale {
            f = 0
            e += 1
        }
        return UInt32(e) << UInt32(m) | UInt32(f)
    }
}

/// One step of a chain as Metal defines it (an M4 Max's generateMipmaps): destination texel x is one bilinear tap at
/// (x + 1/2) s / d - 1/2 in the level above, weights at 1/256, clamped to the edge, sRGB filtered in linear space
enum MipReference {
    static func tap(_ x: Int, _ s: Int, _ d: Int) -> (Int, Int, Double) {
        let u = (Double(x) + 0.5) * Double(s) / Double(d) - 0.5
        let f = floor(u)
        var w = (u - f) * 256
        w.round(.toNearestOrAwayFromZero)
        let i0 = min(max(Int(f), 0), s - 1), i1 = min(max(Int(f) + 1, 0), s - 1)
        return w == 256 ? (i1, i1, 0) : (i0, i1, w / 256)
    }

    static func step(_ above: [UInt8], _ sw: Int, _ sh: Int, _ dw: Int, _ dh: Int, _ layout: MipLayout) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: dw * dh * layout.bytes)
        let xs = (0..<dw).map { tap($0, sw, dw) }, ys = (0..<dh).map { tap($0, sh, dh) }
        for y in 0..<dh {
            let (y0, y1, wy) = ys[y]
            for x in 0..<dw {
                let (x0, x1, wx) = xs[x]
                for f in layout.fields {
                    let v = { (tx: Int, ty: Int) in layout.value(layout.code(above, texel: ty * sw + tx, field: f), f) }
                    let top = v(x0, y0) * (1 - wx) + v(x1, y0) * wx, bottom = v(x0, y1) * (1 - wx) + v(x1, y1) * wx
                    layout.put(layout.code(of: top * (1 - wy) + bottom * wy, f), &out, texel: y * dw + x, field: f)
                }
            }
        }
        return out
    }
}

/// Codes compared field by field: equal, one apart, further apart, and the largest distance
struct MipTally {
    var exact = 0, withinOne = 0, wrong = 0, maxDiff: UInt32 = 0

    /// "exact", "near" (within one code) or "wrong"; "-" with nothing compared
    var verdict: String { wrong > 0 ? "wrong" : withinOne > 0 ? "near" : exact > 0 ? "exact" : "-" }

    mutating func add(_ got: [UInt8], _ want: [UInt8], texels: Int, _ layout: MipLayout) {
        for t in 0..<texels {
            for f in layout.fields {
                let a = layout.code(got, texel: t, field: f), b = layout.code(want, texel: t, field: f)
                let d = a > b ? a - b : b - a
                if d == 0 { exact += 1 } else if d == 1 { withinOne += 1 } else { wrong += 1 }
                maxDiff = max(maxDiff, d)
            }
        }
    }
}

/// How a 2:1 step rounds 8-bit codes: of the four codes' sums with a remainder of 1, 2 or 3 (a quarter, half, three
/// quarters past a code), how many came out at the lower code. Truncation lowers all three; rounding to nearest lowers the
/// quarters and, with ties to the lower code, the halves; an M4 Max's generateMipmaps lowers about half of the halves
final class MipRounding {
    var seen = [0, 0, 0], low = [0, 0, 0]

    func add(_ above: [UInt8], _ sw: Int, _ level: [UInt8], _ dw: Int, _ dh: Int, _ layout: MipLayout) {
        for f in layout.fields where f.kind == .unorm && f.width == 8 {
            for y in 0..<dh {
                for x in 0..<dw {
                    var sum: UInt32 = 0
                    for (ox, oy) in [(0, 0), (1, 0), (0, 1), (1, 1)] {
                        sum += layout.code(above, texel: (2 * y + oy) * sw + 2 * x + ox, field: f)
                    }
                    guard sum % 4 != 0 else { continue }
                    seen[Int(sum % 4) - 1] += 1
                    if layout.code(level, texel: y * dw + x, field: f) == sum / 4 { low[Int(sum % 4) - 1] += 1 }
                }
            }
        }
    }

    var rule: String {
        guard seen.allSatisfy({ $0 > 0 }) else { return "-" }
        let all = (0..<3).map { low[$0] == seen[$0] }, none = (0..<3).map { low[$0] == 0 }
        if all[0] && all[1] && all[2] { return "truncate" }
        if all[0] && all[1] && none[2] { return "nearest_ties_down" }
        if all[0] && none[1] && none[2] { return "nearest_ties_up" }
        if all[0] && none[2] { return "nearest" }
        return "mixed"
    }
}
