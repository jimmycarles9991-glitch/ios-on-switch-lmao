import Foundation

/// gpu.compute.exact_round's references: Metal's float packs and its extended-range codes, each the rule an M4 Max
/// follows on every value of the test.
enum ExactRoundModel {
    /// A pack's code of a value clamped to [0, 1] ([-1, 1] signed), NaN 0: the exact product with the top code rounded
    /// to nearest even
    static func code(_ v: Float, bits: Int, signed: Bool) -> Int {
        if v.isNaN { return 0 }
        let top = Double((1 << (signed ? bits - 1 : bits)) - 1)
        return Int((min(max(Double(v), signed ? -1 : 0), 1) * top).rounded(.toNearestOrEven))
    }

    /// The five words exact_round_packs writes for v: unorm4x8, snorm4x8, unorm2x16, snorm2x16, unorm10a2
    static func words(_ v: Float) -> [UInt32] {
        let u8 = UInt32(code(v, bits: 8, signed: false)), u16 = UInt32(code(v, bits: 16, signed: false))
        let s8 = UInt32(UInt8(bitPattern: Int8(code(v, bits: 8, signed: true))))
        let s16 = UInt32(UInt16(bitPattern: Int16(code(v, bits: 16, signed: true))))
        let u10 = UInt32(code(v, bits: 10, signed: false)), a2 = UInt32(code(v, bits: 2, signed: false))
        return [u8 * 0x0101_0101, s8 * 0x0101_0101, u16 | u16 << 16, s16 | s16 << 16, u10 * 0x0010_0401 | a2 << 30]
    }

    /// Each pack's code boundaries (k + 0.5) / top and 1 and 2 floats either side, the codes' own values, and specials
    static let values: [Float] = {
        var out: [Float] = [.nan, .infinity, -.infinity, -0.0, 2, -2, 0.5, -0.5]
        let near = { (x: Double) -> [Float] in
            let f = Float(x)
            return [f.nextDown.nextDown, f.nextDown, f, f.nextUp, f.nextUp.nextUp]
        }
        for (top, stride) in [(255, 1), (1023, 1), (3, 1), (65535, 61)] {
            for k in Swift.stride(from: 0, to: top, by: stride) {
                out += near((Double(k) + 0.5) / Double(top)) + [Float(Double(k) / Double(top))]
            }
        }
        for (top, stride) in [(127, 1), (32767, 31)] {
            for k in Swift.stride(from: -top, to: top, by: stride) {
                out += near((Double(k) + 0.5) / Double(top)) + [Float(Double(k) / Double(top))]
            }
        }
        return out
    }()

    /// An extended-range code's value, and the same as a half
    static func xr(_ code: Int) -> Float { Float(Double(code - 384) / 510) }
    static func xrHalf(_ code: Int) -> UInt16 { Float16(Double(code - 384) / 510).bitPattern }
}
