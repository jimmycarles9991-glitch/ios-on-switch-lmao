import Foundation
import HWTestCore
import XCTest

/// IconEdgeBand on masks made up to show each case: a symmetric rounded square, the same with its last two columns or
/// rows left unwritten (boot 22's `short right 2` and `short bottom 2`), and one edge texel off by antialiasing only.
final class IconEdgeBandTests: XCTestCase {
    func roundedSquare(_ side: Int, radius: Double) -> [UInt8] {
        (0..<side * side).map { p in
            let x = Double(p % side) + 0.5, y = Double(p / side) + 0.5, n = Double(side)
            let dx = max(radius - x, x - (n - radius), 0), dy = max(radius - y, y - (n - radius), 0)
            let outside = (dx * dx + dy * dy).squareRoot() - radius
            return UInt8((min(max(0.5 - outside, 0), 1) * 255).rounded())
        }
    }

    func testASymmetricMaskHasNoAsymmetry() {
        for side in [30, 56, 80, 136] {
            XCTAssertEqual(
                IconEdgeBand.asymmetry(roundedSquare(side, radius: Double(side) * 0.2237), side: side).field, "0/0")
        }
    }

    func testUnwrittenRightAndBottomBandsAreCounted() {
        let side = 56
        var mask = roundedSquare(side, radius: 12.5)
        for y in 0..<side { for x in side - 2..<side { mask[y * side + x] = 0 } }
        for x in 0..<side { for y in side - 2..<side { mask[y * side + x] = 0 } }
        let found = IconEdgeBand.asymmetry(mask, side: side)
        XCTAssertGreaterThan(found.right, side)
        XCTAssertGreaterThan(found.bottom, side)
    }

    func testAntialiasingOffByAFewCodesIsNotCounted() {
        let side = 56
        var mask = roundedSquare(side, radius: 12.5)
        mask[20 * side + side - 1] &-= 10
        XCTAssertEqual(IconEdgeBand.asymmetry(mask, side: side).field, "0/0")
    }

    func testAlphaIsEveryFourthByte() {
        XCTAssertEqual(IconEdgeBand.alpha([1, 2, 3, 4, 5, 6, 7, 8]), [4, 8])
    }
}
