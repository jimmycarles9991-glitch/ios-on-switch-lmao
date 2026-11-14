import HWTestCore
import XCTest

final class InterlockSpinTests: XCTestCase {
    func testEveryRunMustStayWithinFourTimesTheBaselineMedian() {
        XCTAssertTrue(InterlockSpinBound.accepts([1, 4, 2], baseline: [1, 100, 1]))
        XCTAssertFalse(InterlockSpinBound.accepts([1, 4.001, 2], baseline: [1, 100, 1]))
    }

    func testMissingAndNonGpuTimesCannotPass() {
        for invalid: [Double] in [[], [0], [-1], [.nan], [.infinity]] {
            XCTAssertFalse(InterlockSpinBound.accepts(invalid, baseline: [1]))
            XCTAssertFalse(InterlockSpinBound.accepts([1], baseline: invalid))
        }
    }
}
