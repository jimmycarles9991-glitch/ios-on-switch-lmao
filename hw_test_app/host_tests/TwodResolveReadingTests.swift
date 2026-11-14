import Foundation
import HWTestCore
import XCTest

/// gpu.raw.resolve_twod's model of the 2D engine's 2x box against the arithmetic raw_resolve_rule_test.cpp fits to the
/// device's counts
final class TwodResolveReadingTests: XCTestCase {
    func testCodesAreFifteenBitsNearest() {
        for c in UInt32(0)...255 {
            XCTAssertEqual(X1TwodResolveReading.code15(c), UInt32((Double(c) * 32768 / 255).rounded()))
        }
    }

    func testTiesLandEitherSideAndBothReadingsMissTogether() {
        var both = 0, boxTiesUp = 0
        for a in UInt32(0)...255 {
            for b in UInt32(0)...255 {
                let metal = (a + b + 1) / 2
                let box = X1TwodResolveReading.box(a, b), complement = X1TwodResolveReading.complement(a, b)
                XCTAssertLessThanOrEqual(metal - box, 1)
                XCTAssertLessThanOrEqual(metal - complement, 1)
                if (a + b) % 2 == 0 { XCTAssertEqual(box, metal) }
                if (a + b) % 2 == 1 && box == metal { boxTiesUp += 1 }
                if box != metal && complement != metal { both += 1 }
            }
        }
        XCTAssertEqual(boxTiesUp, 896)
        XCTAssertEqual(both, 896)
        XCTAssertEqual(X1TwodResolveReading.box(0, 1), 0)
        XCTAssertEqual(X1TwodResolveReading.complement(0, 1), 1)
    }
}
