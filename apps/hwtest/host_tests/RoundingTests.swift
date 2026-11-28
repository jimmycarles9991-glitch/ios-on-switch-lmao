import Foundation
import HWTestCore
import XCTest

/// The rounding rules the 2D-engine probes hold the chip's texels to.
final class RoundingTests: XCTestCase {
    func testUnormTieGoesWhereEachRuleSends() {
        let unorm8 = X1Channel.unorm(bits: 8)
        let tie = 127.5 / 255
        XCTAssertEqual(X1Rounding.nearest.code(tie, unorm8), 128)
        XCTAssertEqual(X1Rounding.tiesDown.code(tie, unorm8), 127)
        XCTAssertEqual(X1Rounding.tiesEven.code(tie, unorm8), 128)
        XCTAssertEqual(X1Rounding.down.code(tie, unorm8), 127)
        XCTAssertEqual(X1Rounding.up.code(tie, unorm8), 128)
        XCTAssertEqual(X1Rounding.down.code(64.0 / 255, unorm8), 64)
        XCTAssertEqual(X1Rounding.nearest.code(2.0, unorm8), 255)
    }

    /// Cut to sixteenths of a code first: a hair past the tie falls back to it and goes down, a sixteenth and a half
    /// past goes up
    func testTruncatedToSixteenthsThenTiesDown() {
        let unorm8 = X1Channel.unorm(bits: 8)
        let rule = X1Rounding.trunc4TiesDown
        XCTAssertEqual(rule.code(127.5 / 255, unorm8), 127)
        XCTAssertEqual(rule.code((127.5 + 1.0 / 32) / 255, unorm8), 127)
        XCTAssertEqual(X1Rounding.tiesDown.code((127.5 + 1.0 / 32) / 255, unorm8), 128)
        XCTAssertEqual(rule.code((127.5 + 1.5 / 16) / 255, unorm8), 128)
        XCTAssertEqual(rule.code(127.25 / 255, unorm8), 127)
        XCTAssertEqual(rule.code(127.75 / 255, unorm8), 128)
    }

    func testSmallFloatCodes() {
        let half = X1Channel.float(mantissa: 10)
        XCTAssertEqual(half.value(0x3c00), 1)
        XCTAssertEqual(half.value(0x3800), 0.5)
        XCTAssertEqual(half.value(0x0001), 0x1p-24)
        let between = 1 + 0x1p-11
        XCTAssertEqual(X1Rounding.tiesEven.code(between, half), 0x3c00)
        XCTAssertEqual(X1Rounding.nearest.code(between, half), 0x3c01)
        XCTAssertEqual(X1Rounding.down.code(1 + 0x1p-10 * 0.9, half), 0x3c00)
        let f11 = X1Channel.float(mantissa: 6)
        XCTAssertEqual(f11.value(14 << 6), 0.5)
        XCTAssertEqual(X1Rounding.tiesEven.code(0.5 + 0x1p-8, f11), 14 << 6)
    }

    func testSrgbRoundsInEncodedSpace() {
        let srgb = X1Channel.srgb8
        for code: UInt32 in [0, 1, 17, 128, 254, 255] {
            let back = X1Channel.encoded(X1Channel.linear(srgb.value(code)))
            XCTAssertEqual(X1Rounding.nearest.code(back, srgb), code)
        }
    }

    func testATallyNamesTheRuleThatFits() {
        var tally = X1RuleTally()
        let channels: [X1Channel] = [.unorm(bits: 8), .unorm(bits: 8)]
        tally.add([127, 64], exact: [127.5 / 255, 64.25 / 255], channels: channels, metal: .nearest, at: 0)
        XCTAssertEqual(tally.offMetal, 1)
        XCTAssertEqual(tally.rules, [.tiesDown, .down, .trunc4TiesDown])
        XCTAssertEqual(tally.text, "near:1:ties_down+down+trunc4_ties_down")
        XCTAssertEqual(tally.fewestMisses, 0)
        tally.add([200, 64], exact: [127.5 / 255, 64.0 / 255], channels: channels, metal: .nearest, at: 1)
        XCTAssertEqual(tally.grade, "wrong")
        XCTAssertEqual(tally.rules, [])
    }

    func testFloatValueAndBracketMatchReferenceBitForBit() {
        func refValue(_ code: UInt32, _ m: Int) -> Double {
            let exponent = Int(code >> UInt32(m)) & 31, fraction = Double(code & ((1 << UInt32(m)) - 1))
            let one = Double(1 << m)
            return exponent == 0 ? fraction / one * pow(2, -14) : (1 + fraction / one) * pow(2, Double(exponent - 15))
        }
        func refBracket(_ v: Double, _ m: Int) -> (UInt32, UInt32) {
            var low: UInt32 = 0
            var step: UInt32 = 1 << UInt32(m + 4)
            while step > 0 {
                if refValue(low + step, m) <= v { low += step }
                step >>= 1
            }
            return (low, low + 1)
        }
        for m in [5, 6, 10] {
            let ch = X1Channel.float(mantissa: m)
            let count = UInt32(1 << (m + 5))
            for code in 0 ..< count {
                XCTAssertEqual(ch.value(code).bitPattern, refValue(code, m).bitPattern, "m=\(m) code=\(code)")
            }
            var probes: [Double] = [-1.0, -0x1p-30, 0.0, 0x1p-30, 0x1p-24, 0x1p-14, 1.0, 65504.0, 1e6, .infinity, .nan]
            let strideStep = max(1, Int(count / 256))
            for code in stride(from: 0, to: Int(count) - 1, by: strideStep) {
                let a = refValue(UInt32(code), m), b = refValue(UInt32(code + 1), m)
                probes.append(a)
                probes.append((a + b) * 0.5)
                probes.append(a.nextUp)
                probes.append(b.nextDown)
            }
            for v in probes {
                let got = ch.bracket(v), want = refBracket(v, m)
                XCTAssertEqual(got.0, want.0, "m=\(m) v=\(v)")
                XCTAssertEqual(got.1, want.1, "m=\(m) v=\(v)")
            }
        }
    }

    func testRuleCodesAndMergeMatchSerialTally() {
        let channels: [X1Channel] = [.unorm(bits: 8), .srgb8, .float(mantissa: 10), .float(mantissa: 6)]
        let samples: [(SIMD4<UInt32>, SIMD4<Double>)] = [
            (SIMD4(127, 64, 0x3c00, 14 << 6), SIMD4(127.5 / 255, 64.25 / 255, 1 + 0x1p-11, 0.5 + 0x1p-8)),
            (SIMD4(200, 128, 0x3c02, 15 << 6), SIMD4(127.5 / 255, 127.6 / 255, 1 + 0x1p-11, 1.0)),
            (SIMD4(0, 255, 0x0001, 0), SIMD4(0, 1, 0x1p-24, 0)),
        ]
        var serial = X1RuleTally(), simd = X1RuleTally(), precomputed = X1RuleTally()
        var chunk0 = X1RuleTally(), chunk1 = X1RuleTally()
        for (i, (got, exact)) in samples.enumerated() {
            let gotArr = [got.x, got.y, got.z, got.w]
            let exactArr = [exact.x, exact.y, exact.z, exact.w]
            serial.add(gotArr, exact: exactArr, channels: channels, metal: .nearest, at: i)
            simd.add(got, exact: exact, channels: channels, metal: .nearest, at: i)
            let codes = (0 ..< 4).map { X1RuleCodes(exact[$0], channels[$0], metal: .nearest) }
            precomputed.add(got, expected: codes, at: i)
            if i < 1 {
                chunk0.add(got, expected: codes, at: i)
            } else {
                chunk1.add(got, expected: codes, at: i)
            }
        }
        chunk0.merge(chunk1)
        for other in [simd, precomputed, chunk0] {
            XCTAssertEqual(other.texels, serial.texels)
            XCTAssertEqual(other.offMetal, serial.offMetal)
            XCTAssertEqual(other.beyondOne, serial.beyondOne)
            XCTAssertEqual(other.first, serial.first)
            XCTAssertEqual(other.misses, serial.misses)
            XCTAssertEqual(other.rules, serial.rules)
            XCTAssertEqual(other.fewestMisses, serial.fewestMisses)
            XCTAssertEqual(other.text, serial.text)
        }
    }
}
