import HWTestCore
import XCTest

final class X1Unorm10Tests: XCTestCase {
    func testOddHeightPadsToEvenQuadRows() {
        XCTAssertEqual(X1Unorm10Layout.paddedRows(17), 18)
        XCTAssertEqual(X1Unorm10Layout.paddedRows(16), 16)
        XCTAssertEqual(X1Unorm10Layout.paddedRows(1), 2)
        XCTAssertEqual(X1Unorm10Layout.paddedElementCount(width: 1024, height: 17), 1024 * 18)
    }

    func testHelperRowIndexClampsInsideBuffer() {
        let width = 1024
        let height = 17
        let limit = width * height
        for x in [0, 1, 511, 1023, 1024] {
            let index = X1Unorm10Layout.clampedIndex(x: x, y: height, width: width, height: height)
            XCTAssertGreaterThanOrEqual(index, 0)
            XCTAssertLessThan(index, limit)
            XCTAssertEqual(index, (height - 1) * width + min(x, width - 1))
        }
    }
}
