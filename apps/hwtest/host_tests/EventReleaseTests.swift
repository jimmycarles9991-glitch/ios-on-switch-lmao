import HWTestCore
import XCTest

final class EventReleaseTests: XCTestCase {
    func testFmaLoopBufferBytesMatchesFloat4PerThread() {
        XCTAssertEqual(EventReleaseCheck.fmaLoopBufferBytes(threads: 1 << 14), 262_144)
        XCTAssertEqual(EventReleaseCheck.fmaLoopBufferBytes(threads: 1), 16)
        XCTAssertEqual(EventReleaseCheck.fmaLoopBufferBytes(threads: 0), 0)
    }

    func testWaitersPassedSkipsBothSoloRounds() {
        let rounds = [
            EventReleaseCheck.WaiterOutcome(waited: false, done: false, wrongBytes: 0),
            EventReleaseCheck.WaiterOutcome(waited: true, done: true, wrongBytes: 0),
            EventReleaseCheck.WaiterOutcome(waited: true, done: true, wrongBytes: 0),
            EventReleaseCheck.WaiterOutcome(waited: false, done: false, wrongBytes: 0),
            EventReleaseCheck.WaiterOutcome(waited: true, done: true, wrongBytes: 0),
            EventReleaseCheck.WaiterOutcome(waited: true, done: true, wrongBytes: 0),
        ]
        XCTAssertTrue(EventReleaseCheck.waitersPassed(rounds, expectedWaited: 4))
        XCTAssertFalse(EventReleaseCheck.waitersPassed(rounds, expectedWaited: 5))
    }

    func testWaitersPassedFailsOnIncompleteOrCorruptWaiter() {
        let base = [
            EventReleaseCheck.WaiterOutcome(waited: false, done: false, wrongBytes: 0),
            EventReleaseCheck.WaiterOutcome(waited: true, done: true, wrongBytes: 0),
            EventReleaseCheck.WaiterOutcome(waited: true, done: true, wrongBytes: 0),
            EventReleaseCheck.WaiterOutcome(waited: false, done: false, wrongBytes: 0),
            EventReleaseCheck.WaiterOutcome(waited: true, done: true, wrongBytes: 0),
            EventReleaseCheck.WaiterOutcome(waited: true, done: true, wrongBytes: 0),
        ]
        var unfinished = base
        unfinished[4] = EventReleaseCheck.WaiterOutcome(waited: true, done: false, wrongBytes: 0)
        XCTAssertFalse(EventReleaseCheck.waitersPassed(unfinished, expectedWaited: 4))

        var corrupt = base
        corrupt[5] = EventReleaseCheck.WaiterOutcome(waited: true, done: true, wrongBytes: 65536)
        XCTAssertFalse(EventReleaseCheck.waitersPassed(corrupt, expectedWaited: 4))
    }
}
