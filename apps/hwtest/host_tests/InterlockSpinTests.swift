import HWTestCore
import XCTest

final class InterlockSpinTests: XCTestCase {
    func testEveryRunMustStayWithinItsLayersTimesTheLoneLayersMedian() {
        XCTAssertTrue(InterlockSpinBound.accepts([1, 4, 2], layers: 4, lone: [1, 100, 1]))
        XCTAssertFalse(InterlockSpinBound.accepts([1, 4.001, 2], layers: 4, lone: [1, 100, 1]))
    }

    /// Boot 2's medians (1280x720 RGBA16Float, full-screen layers): the interlocked arm 2.001, 6.326 and 24.665 ms
    /// for 1, 4 and 16 layers, which the old bound of 4x the ROP arm (1.003, 1.478, 5.432 ms) refused at 4 and 16
    func testBoot2sInterlockedLayersCostNoMoreThanLoneOnes() {
        XCTAssertTrue(InterlockSpinBound.accepts([6.326], layers: 4, lone: [2.001]))
        XCTAssertTrue(InterlockSpinBound.accepts([24.665], layers: 16, lone: [2.001]))
        XCTAssertFalse(InterlockSpinBound.accepts([8.1], layers: 4, lone: [2.001]))
    }

    func testMissingAndNonGpuTimesCannotPass() {
        for invalid: [Double] in [[], [0], [-1], [.nan], [.infinity]] {
            XCTAssertFalse(InterlockSpinBound.accepts(invalid, layers: 4, lone: [1]))
            XCTAssertFalse(InterlockSpinBound.accepts([1], layers: 4, lone: invalid))
        }
        XCTAssertFalse(InterlockSpinBound.accepts([1], layers: 0, lone: [1]))
    }
}
