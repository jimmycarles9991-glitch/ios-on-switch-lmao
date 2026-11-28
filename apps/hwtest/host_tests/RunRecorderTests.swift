import Foundation
import HWTestCore
import os
import XCTest

/// 25 Nov 19:18:35: HW Test's main thread hung 329 ms deciding the person's run choice, 218 ms of it decoding its
/// 928 KiB report and 45 ms in the report's F_FULLFSYNC (recorder dump 3). The recorder's queue now does that work
/// while the main thread awaits it; the card sees the calls in the order they were made.
final class RunRecorderTests: XCTestCase {
    var store: RunStore!
    var recorder: RunRecorder!

    override func setUpWithError() throws {
        store = RunStore(directory: FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString))
        recorder = RunRecorder(store: store, log: Logger(subsystem: "host-test", category: "recorder"))
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: store.directory)
    }

    func record(_ id: String) -> TestRecord {
        TestRecord(
            id: id, subsystem: "System", name: id, interactive: false, outcome: .pass(), milliseconds: 2,
            finished: Date(timeIntervalSince1970: 10)
        )
    }

    func planned(_ ids: [String], started: TimeInterval = 0) -> Report {
        var report = Report(bootUUID: "B1", ios: 27, started: Date(timeIntervalSince1970: started), identity: [:])
        report.plan(ids)
        return report
    }

    func testARunsLinesLandInTheOrderTheyWereMadeAndTheReadSeesTheClosedRun() async throws {
        recorder.start(planned(["a", "b"]))
        _ = await recorder.advance(nil, begin: "a")
        recorder.interrupt("backgrounded")
        recorder.interrupt(nil)
        _ = await recorder.advance(record("a"), begin: "b")
        _ = await recorder.advance(record("b"), begin: nil)
        let finished = await recorder.finish(at: Date(timeIntervalSince1970: 20))
        let final = try XCTUnwrap(finished)
        XCTAssertEqual(final.run.status, .done)
        XCTAssertEqual(final.results.map(\.id), ["a", "b"])
        XCTAssertNil(final.run.interruption)
        let recent = await recorder.recent(boot: "B1")
        XCTAssertEqual(recent, [final], "the read comes after the close on the same queue")
    }

    func testAContinuedRunIsOnTheCardBeforeTheCheckpointReturns() async throws {
        var continued = planned(["a", "b"])
        continued.begin("a")
        continued.record(record("a"))
        try await recorder.checkpoint(continued)
        let read = await recorder.recent(boot: "B1")
        XCTAssertEqual(read.last?.results.map(\.id), ["a"])
        XCTAssertEqual(read.last?.run.status, .running)
    }

    func testACancelledRunIsClosedAndItsJournalGone() async throws {
        var running = planned(["a"])
        try await recorder.checkpoint(running)
        running.cancel(at: Date(timeIntervalSince1970: 30))
        try await recorder.close(running)
        let names = try FileManager.default.contentsOfDirectory(atPath: store.directory.path)
        XCTAssertEqual(names, [running.run.id + ".json"])
        let read = await recorder.recent(boot: "B1")
        XCTAssertEqual(read.last?.run.status, .cancelled)
        XCTAssertNil(Report.interrupted(read), "a cancelled run is offered again by no launch")
    }

    func testAChoiceTheCardRefusedThrowsToTheCaller() async throws {
        try "not a directory".write(to: store.directory, atomically: true, encoding: .utf8)
        var refused: Error?
        do {
            try await recorder.close(planned(["a"]))
        } catch {
            refused = error
        }
        XCTAssertNotNil(refused, "the close went through a file where the directory should be")
    }
}
