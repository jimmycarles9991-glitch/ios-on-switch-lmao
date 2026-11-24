import Foundation
import HWTestCore
import XCTest

final class BarrierPatternTests: XCTestCase {
    /// X1Probes.metal's x1_hash and x1_seeded_* worked apart from this code, at two texels and seeds.
    func testEachFormatHoldsTheBytesTheShaderStores() {
        XCTAssertEqual(BarrierPattern.bgra8.texel(3, 5, seed: 7), [210, 2, 186, 25])
        XCTAssertEqual(BarrierPattern.rgba16f.texel(3, 5, seed: 7), [186, 62, 128, 60, 157, 61, 103, 60])
        XCTAssertEqual(
            BarrierPattern.rgba32f.texel(3, 5, seed: 7),
            [186, 2, 210, 63, 87, 64, 186, 63, 10, 72, 231, 63, 1, 233, 140, 63]
        )
        XCTAssertEqual(BarrierPattern.bgra8.texel(1279, 719, seed: 121), [227, 109, 187, 223])
        XCTAssertEqual(BarrierPattern.rgba16f.texel(1279, 719, seed: 121), [187, 61, 219, 60, 254, 61, 127, 63])
        XCTAssertEqual(
            BarrierPattern.rgba32f.texel(1279, 719, seed: 121),
            [187, 109, 227, 63, 183, 109, 252, 63, 182, 141, 255, 63, 182, 241, 239, 63]
        )
    }

    func testAnImageIsItsTexelsRowByRow() {
        for pattern in [BarrierPattern.bgra8, .rgba16f, .rgba32f] {
            let texels = (0 ..< 5).flatMap { y in (0 ..< 37).flatMap { x in pattern.texel(x, y, seed: 9) } }
            XCTAssertEqual(pattern.image(width: 37, height: 5, seed: 9), texels)
        }
    }

    func testTheFirstDifferenceIsTheFirstByteOffOrTheShorterOnesEnd() {
        XCTAssertNil(firstDifference([1, 2, 3], [1, 2, 3]))
        XCTAssertNil(firstDifference([], []))
        XCTAssertEqual(firstDifference([1, 2, 3, 4], [1, 9, 3, 5]), 1)
        XCTAssertEqual(firstDifference([1, 2, 3, 4], [1, 2, 3, 5]), 3)
        XCTAssertEqual(firstDifference([1, 2], [1, 2, 3]), 2)
    }
}
