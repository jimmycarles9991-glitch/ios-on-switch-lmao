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
}
