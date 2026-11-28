import HWTestCore
import XCTest

final class LockAheadBoundTests: XCTestCase {
    /// 25 Nov 14:15: A's render took 28.8 ms on the GPU against a 39.9 ms calibration; half of A's is the bound.
    func testTheBoundIsHalfOfAsOwnGPUTime() {
        XCTAssertEqual(LockAheadBound.requiredMs(aGpuMs: 28.8, calibratedMs: 39.9), 14.4, accuracy: 1e-9)
        XCTAssertEqual(LockAheadBound.requiredMs(aGpuMs: 40.2, calibratedMs: 39.9), 20.1, accuracy: 1e-9)
    }

    func testTheCalibrationStandsInWithoutStamps() {
        XCTAssertEqual(LockAheadBound.requiredMs(aGpuMs: 0, calibratedMs: 39.9), 19.95, accuracy: 1e-9)
    }
}
