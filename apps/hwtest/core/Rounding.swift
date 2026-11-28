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
    @inline(__always)
    public func value(_ code: UInt32) -> Double {
        switch self {
        case .unorm(let bits): return Double(code) / Double((1 << bits) - 1)
        case .srgb8: return Double(code) / 255
        case .float(let m):
            let exponent = Int(code >> UInt32(m)) & 31
            let fraction = Double(code & ((1 << UInt32(m)) - 1))
            if exponent == 0 {
                return fraction * Double(bitPattern: UInt64(1023 - 14 - m) << 52)
            }
            return (Double(1 << m) + fraction) * Double(bitPattern: UInt64(1023 + exponent - 15 - m) << 52)
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
    @inline(__always)
    public func bracket(_ v: Double) -> (UInt32, UInt32) {
        switch self {
        case .unorm, .srgb8:
            let bits = codeBits ?? 8
            let x = min(max(v, 0), 1) * Double((1 << bits) - 1)
            let low = UInt32(x.rounded(.down))
            return (low, min(low + 1, UInt32((1 << bits) - 1)))
        case .float(let m):
            guard v > 0 else { return (0, 1) }
            let maxCode = UInt32((1 << (m + 5)) - 1)
            if v >= value(maxCode) { return (maxCode, maxCode + 1) }
            if v < 0x1p-14 {
                let low = UInt32(v * Double(bitPattern: UInt64(1023 + 14 + m) << 52))
                return (low, low + 1)
            }
            let bits = v.bitPattern
            let exp = Int((bits >> 52) & 0x7ff) - 1023
            let frac = UInt32((bits & 0x000f_ffff_ffff_ffff) >> UInt64(52 - m))
            let low = (UInt32(exp + 15) << UInt32(m)) | frac
            return (low, low + 1)
        case .float32:
            let f = Float(v)
            let low = Double(f) <= v ? f : f.nextDown
            return (low.bitPattern, low.nextUp.bitPattern)
        }
    }
}

/// Every X1Rounding's code for one (exact value, channel) pair, computed from a single bracket lookup.
public struct X1RuleCodes: Sendable, Equatable {
    public let nearest: UInt32
    public let tiesDown: UInt32
    public let tiesEven: UInt32
    public let down: UInt32
    public let up: UInt32
    public let trunc4TiesDown: UInt32
    public let metal: UInt32

    @inline(__always)
    public init(_ v: Double, _ channel: X1Channel, metal: X1Rounding) {
        let (low, high) = channel.bracket(v)
        let a = channel.value(low), b = channel.value(high)
        if a == v || high == low {
            (nearest, tiesDown, tiesEven, down, up, trunc4TiesDown, self.metal) = (low, low, low, low, low, low, low)
            return
        }
        down = low
        up = high
        let d = (v - a) - (b - v)
        nearest = d < 0 ? low : high
        tiesDown = d > 0 ? high : low
        tiesEven = d < 0 ? low : d > 0 ? high : (low & 1 == 0 ? low : high)
        if let bits = channel.codeBits {
            let sixteenths = (min(max(v, 0), 1) * Double((1 << bits) - 1) * 16).rounded(.down)
            trunc4TiesDown = sixteenths - Double(low) * 16 > 8 ? high : low
        } else {
            trunc4TiesDown = tiesDown
        }
        switch metal {
        case .nearest: self.metal = nearest
        case .tiesDown: self.metal = tiesDown
        case .tiesEven: self.metal = tiesEven
        case .down: self.metal = down
        case .up: self.metal = up
        case .trunc4TiesDown: self.metal = trunc4TiesDown
        }
    }
}

/// How a unit turns an exact value into a code.
public enum X1Rounding: String, CaseIterable, Sendable {
    case nearest // ties away from zero: Metal's unorm rule
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
    private var missesNearest = 0, missesTiesDown = 0, missesTiesEven = 0
    private var missesDown = 0, missesUp = 0, missesTrunc4 = 0
    public var first = ""

    public init() {}

    public var misses: [X1Rounding: Int] {
        get {
            var out: [X1Rounding: Int] = [:]
            if missesNearest != 0 { out[.nearest] = missesNearest }
            if missesTiesDown != 0 { out[.tiesDown] = missesTiesDown }
            if missesTiesEven != 0 { out[.tiesEven] = missesTiesEven }
            if missesDown != 0 { out[.down] = missesDown }
            if missesUp != 0 { out[.up] = missesUp }
            if missesTrunc4 != 0 { out[.trunc4TiesDown] = missesTrunc4 }
            return out
        }
        set {
            missesNearest = newValue[.nearest] ?? 0
            missesTiesDown = newValue[.tiesDown] ?? 0
            missesTiesEven = newValue[.tiesEven] ?? 0
            missesDown = newValue[.down] ?? 0
            missesUp = newValue[.up] ?? 0
            missesTrunc4 = newValue[.trunc4TiesDown] ?? 0
        }
    }

    @inline(__always)
    private mutating func addChannel(_ got: UInt32, codes: X1RuleCodes, at t: Int, channel c: Int, off: inout Bool) {
        if got != codes.metal {
            off = true
            if abs(Int64(got) - Int64(codes.metal)) > 1 { beyondOne += 1 }
            if first.isEmpty { first = "\(t).\(c):\(got),want_\(codes.metal)" }
        }
        if got != codes.nearest { missesNearest += 1 }
        if got != codes.tiesDown { missesTiesDown += 1 }
        if got != codes.tiesEven { missesTiesEven += 1 }
        if got != codes.down { missesDown += 1 }
        if got != codes.up { missesUp += 1 }
        if got != codes.trunc4TiesDown { missesTrunc4 += 1 }
    }

    /// `exact`: per channel, the exact value the unit should have rounded
    public mutating func add(_ got: [UInt32], exact: [Double], channels: [X1Channel], metal: X1Rounding, at t: Int) {
        texels += 1
        var off = false
        for (c, channel) in channels.enumerated() {
            addChannel(got[c], codes: X1RuleCodes(exact[c], channel, metal: metal), at: t, channel: c, off: &off)
        }
        if off { offMetal += 1 }
    }

    @inline(__always)
    public mutating func add(
        _ got: SIMD4<UInt32>, exact: SIMD4<Double>, channels: [X1Channel], metal: X1Rounding, at t: Int
    ) {
        texels += 1
        var off = false
        for c in 0 ..< channels.count {
            addChannel(got[c], codes: X1RuleCodes(exact[c], channels[c], metal: metal), at: t, channel: c, off: &off)
        }
        if off { offMetal += 1 }
    }

    @inline(__always)
    public mutating func add(_ got: SIMD4<UInt32>, expected: [X1RuleCodes], at t: Int) {
        texels += 1
        var off = false
        for c in 0 ..< expected.count {
            addChannel(got[c], codes: expected[c], at: t, channel: c, off: &off)
        }
        if off { offMetal += 1 }
    }

    /// Folds a later chunk's tally into this one, keeping the earliest `first` mismatch.
    public mutating func merge(_ other: X1RuleTally) {
        texels += other.texels
        offMetal += other.offMetal
        beyondOne += other.beyondOne
        missesNearest += other.missesNearest
        missesTiesDown += other.missesTiesDown
        missesTiesEven += other.missesTiesEven
        missesDown += other.missesDown
        missesUp += other.missesUp
        missesTrunc4 += other.missesTrunc4
        if first.isEmpty { first = other.first }
    }

    public var rules: [X1Rounding] {
        var out: [X1Rounding] = []
        if missesNearest == 0 { out.append(.nearest) }
        if missesTiesDown == 0 { out.append(.tiesDown) }
        if missesTiesEven == 0 { out.append(.tiesEven) }
        if missesDown == 0 { out.append(.down) }
        if missesUp == 0 { out.append(.up) }
        if missesTrunc4 == 0 { out.append(.trunc4TiesDown) }
        return out
    }

    /// The fewest channels any one rule misses
    public var fewestMisses: Int {
        min(
            min(missesNearest, missesTiesDown),
            min(min(missesTiesEven, missesDown), min(missesUp, missesTrunc4))
        )
    }

    /// exact (Metal's rule everywhere), near (off it by one code at most), wrong
    public var grade: String { offMetal == 0 ? "exact" : beyondOne == 0 ? "near" : "wrong" }

    /// "<grade>:<texels off Metal>:<rules that fit, '+' joined, or none>"
    public var text: String {
        "\(grade):\(offMetal):\(rules.isEmpty ? "none" : rules.map(\.rawValue).joined(separator: "+"))"
    }
}
