import Foundation

/// A colour format's channels as codes (RGBA order) and the values they mean, for probes that hold the GPU's texels to
/// CPU rules: the average or filter of known codes, rounded by each rule a fixed-function unit might use.
public enum X1Channel: Sendable, Equatable {
    case unorm(bits: Int)
    case srgb8
    /// Unsigned or signed float with a 5-bit exponent (bias 15): half (10), and RG11B10's 6 and 5
    case float(mantissa: Int)
    case float32

    /// The code's value; an sRGB code's is its encoded value (code / 255), the space its rounding happens in
    public func value(_ code: UInt32) -> Double {
        switch self {
        case .unorm(let bits): return Double(code) / Double((1 << bits) - 1)
        case .srgb8: return Double(code) / 255
        case .float(let m):
            let exponent = Int(code >> UInt32(m)) & 31, fraction = Double(code & ((1 << UInt32(m)) - 1))
            let one = Double(1 << m)
            return exponent == 0 ? fraction / one * pow(2, -14) : (1 + fraction / one) * pow(2, Double(exponent - 15))
        case .float32: return Double(Float(bitPattern: code))
        }
    }

    /// A unorm or sRGB channel's bits
    public var codeBits: Int? {
        switch self {
        case .unorm(let bits): return bits
        case .srgb8: return 8
        default: return nil
        }
    }

    public static func linear(_ encoded: Double) -> Double {
        encoded <= 0.04045 ? encoded / 12.92 : pow((encoded + 0.055) / 1.055, 2.4)
    }

    public static func encoded(_ linear: Double) -> Double {
        linear <= 0.0031308 ? linear * 12.92 : 1.055 * pow(linear, 1 / 2.4) - 0.055
    }

    /// The codes on either side of a non-negative value: (below or equal, the next one up)
    public func bracket(_ v: Double) -> (UInt32, UInt32) {
        switch self {
        case .unorm, .srgb8:
            let bits = codeBits ?? 8
            let x = min(max(v, 0), 1) * Double((1 << bits) - 1)
            let low = UInt32(x.rounded(.down))
            return (low, min(low + 1, UInt32((1 << bits) - 1)))
        case .float(let m):
            var low: UInt32 = 0
            var step: UInt32 = 1 << UInt32(m + 4)
            while step > 0 {
                if value(low + step) <= v { low += step }
                step >>= 1
            }
            return (low, low + 1)
        case .float32:
            let f = Float(v)
            let low = Double(f) <= v ? f : f.nextDown
            return (low.bitPattern, low.nextUp.bitPattern)
        }
    }
}

/// How a unit turns an exact value into a code.
public enum X1Rounding: String, CaseIterable, Sendable {
    case nearest  // ties away from zero: Metal's unorm rule
    case tiesDown = "ties_down"
    case tiesEven = "ties_even"
    case down
    case up
    /// The ROP's unorm store as the bench measured it: the value cut to four bits below the code, then nearest with
    /// ties down (a float channel: ties down)
    case trunc4TiesDown = "trunc4_ties_down"

    public func code(_ v: Double, _ channel: X1Channel) -> UInt32 {
        let (low, high) = channel.bracket(v)
        let a = channel.value(low), b = channel.value(high)
        if a == v || high == low { return low }
        switch self {
        case .down: return low
        case .up: return high
        case .trunc4TiesDown:
            guard let bits = channel.codeBits else { return X1Rounding.tiesDown.code(v, channel) }
            let sixteenths = (min(max(v, 0), 1) * Double((1 << bits) - 1) * 16).rounded(.down)
            return sixteenths - Double(low) * 16 > 8 ? high : low
        case .nearest, .tiesDown, .tiesEven:
            let d = (v - a) - (b - v)
            if d < 0 { return low }
            if d > 0 { return high }
            return self == .nearest ? high : self == .tiesDown ? low : (low % 2 == 0 ? low : high)
        }
    }
}

/// One cell's texels against Metal's rule and every other rounding: texels off Metal's rule, channels more than one
/// code off it, and per rule the channels it misses (0: the rule the unit followed in this cell).
public struct X1RuleTally: Sendable {
    public var texels = 0, offMetal = 0, beyondOne = 0
    public var misses: [X1Rounding: Int] = [:]
    public var first = ""

    public init() {}

    /// `exact`: per channel, the exact value the unit should have rounded
    public mutating func add(_ got: [UInt32], exact: [Double], channels: [X1Channel], metal: X1Rounding, at t: Int) {
        texels += 1
        var off = false
        for (c, channel) in channels.enumerated() {
            let want = metal.code(exact[c], channel)
            if got[c] != want {
                off = true
                if abs(Int64(got[c]) - Int64(want)) > 1 { beyondOne += 1 }
                if first.isEmpty { first = "\(t).\(c):\(got[c]),want_\(want)" }
            }
            for rule in X1Rounding.allCases where rule.code(exact[c], channel) != got[c] {
                misses[rule, default: 0] += 1
            }
        }
        if off { offMetal += 1 }
    }

    public var rules: [X1Rounding] { X1Rounding.allCases.filter { (misses[$0] ?? 0) == 0 } }

    /// The fewest channels any one rule misses
    public var fewestMisses: Int { X1Rounding.allCases.map { misses[$0] ?? 0 }.min() ?? 0 }

    /// exact (Metal's rule everywhere), near (off it by one code at most), wrong
    public var grade: String { offMetal == 0 ? "exact" : beyondOne == 0 ? "near" : "wrong" }

    /// "<grade>:<texels off Metal>:<rules that fit, '+' joined, or none>"
    public var text: String {
        "\(grade):\(offMetal):\(rules.isEmpty ? "none" : rules.map(\.rawValue).joined(separator: "+"))"
    }
}
