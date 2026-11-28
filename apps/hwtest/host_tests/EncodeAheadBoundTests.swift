import HWTestCore
import XCTest

/// gpu.path.commit_latency's scale_event arm read pipelined=0.0 of 15 encoders through 11-22 (the whole encode ahead
/// given back at its event) and 8.0 from 11-24 (it stops at the event after encoder 8).
final class EncodeAheadBoundTests: XCTestCase {
    func event(_ pipelined: Double) -> String? {
        EncodeAheadBound.failure("scale_event", pipelined: pipelined, encoders: 15, passes: 15, eventAfter: 8)
    }

    func testTheEventShapeEncodesTheEncodersBeforeItsEvent() {
        XCTAssertNil(event(8))
        XCTAssertNil(event(7.5))
    }

    func testAnEncodeAheadGivenBackAtTheEventFails() {
        XCTAssertEqual(event(0), "scale_event: 0.0 of the 8 encoders before the event encoded ahead")
        XCTAssertNotNil(event(6.9))
    }

    func testAnEncodeAheadPastTheEventFails() {
        XCTAssertEqual(event(8.0625), "scale_event: 8.0625 encoders encoded ahead past the event after encoder 8")
    }

    func testAShapeWithoutAnEventLeavesTheCommitAtMostItsLastHandOff() {
        XCTAssertNil(EncodeAheadBound.failure("scale", pipelined: 10, encoders: 15, passes: 15, eventAfter: nil))
        XCTAssertNotNil(EncodeAheadBound.failure("scale", pipelined: 9.9, encoders: 15, passes: 15, eventAfter: nil))
        XCTAssertNil(EncodeAheadBound.failure("one_pass", pipelined: 0, encoders: 1, passes: 1, eventAfter: nil))
    }
}
