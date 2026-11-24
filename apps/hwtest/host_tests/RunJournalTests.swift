import Foundation
import HWTestCore
import XCTest

final class RunJournalTests: XCTestCase {
    let boot = "B1"
    let ids = ["a", "b", "c"]
    var store: RunStore!

    override func setUpWithError() throws {
        store = RunStore(directory: FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString))
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: store.directory.deletingLastPathComponent())
    }

    func record(_ id: String, _ outcome: Outcome = .pass(), at second: TimeInterval) -> TestRecord {
        TestRecord(
            id: id, subsystem: "Storage", name: id, interactive: false, outcome: outcome, milliseconds: 3,
            finished: Date(timeIntervalSince1970: second)
        )
    }

    func planned(boot: String = "B1", started: TimeInterval = 0) -> Report {
        var report = Report(
            bootUUID: boot, ios: 27, started: Date(timeIntervalSince1970: started), identity: ["build": "24B119"]
        )
        report.plan(ids)
        return report
    }

    func uninterrupted() -> Report {
        var report = planned()
        for (index, id) in ids.enumerated() {
            report.begin(id)
            report.record(record(id, at: Double(index + 1)))
        }
        report.finish(at: Date(timeIntervalSince1970: 100))
        return report
    }

    func size(_ url: URL) throws -> Int {
        try FileManager.default.attributesOfItem(atPath: url.path)[.size] as! Int
    }

    func continued(_ report: Report, boot: String = "B1") -> Continuation {
        report.continued(boot: boot, at: Date(timeIntervalSince1970: 50), identity: [:], available: Set(ids)) {
            self.record($0, $1, at: 40)
        }
    }

    func testAJournalReadsBackAsTheRunItRecorded() throws {
        let journal = try store.open(planned())
        try journal.advance(nil, begin: "a")
        try journal.advance(record("a", at: 1), begin: "b")
        try journal.interrupt("backgrounded")
        try journal.interrupt(nil)
        let read = RunJournal.report(from: try Data(contentsOf: journal.url))
        XCTAssertEqual(read, journal.report)
        XCTAssertEqual(read?.run.current, "b")
        XCTAssertEqual(read?.results.map(\.id), ["a"])
        XCTAssertNil(read?.run.interruption)
    }

    func testAKillMidLineLosesThatLineAloneAndTheContinuedRunEndsAsAnUninterruptedOne() throws {
        let journal = try store.open(planned())
        try journal.advance(nil, begin: "a")
        try journal.advance(record("a", at: 1), begin: "b")
        try journal.interrupt("backgrounded")
        let killed = journal.report
        let whole = try size(journal.url)
        try journal.advance(record("b", at: 2), begin: "c")
        let handle = try FileHandle(forWritingTo: journal.url)
        try handle.truncate(atOffset: UInt64(whole + (try size(journal.url) - whole) / 2))
        try handle.close()

        let found = store.recent(boot: boot)
        XCTAssertEqual(found, [killed], "the torn line is left out, every whole one kept")
        let going = continued(found[0])
        XCTAssertEqual(going.cause, .interrupted("b", reason: "backgrounded"))
        XCTAssertEqual(going.order, ["b", "c"])

        let resumed = try store.open(going.report)
        try resumed.advance(nil, begin: "b")
        try resumed.advance(record("b", at: 2), begin: "c")
        try resumed.advance(record("c", at: 3), begin: nil)
        try resumed.finish(at: Date(timeIntervalSince1970: 100))
        try store.close(resumed.report)
        XCTAssertEqual(resumed.report, uninterrupted())
        XCTAssertEqual(store.recent(boot: boot), [uninterrupted()], "read back from the run's json")
        XCTAssertFalse(FileManager.default.fileExists(atPath: resumed.url.path), "a closed run keeps no journal")
    }

    func testAProcessThatDiedInATestRecordsACrashAndARestartRunsTheTestLast() throws {
        let journal = try store.open(planned())
        try journal.advance(nil, begin: "a")
        try journal.advance(record("a", at: 1), begin: "b")
        let read = try XCTUnwrap(store.recent(boot: boot).last)

        let crashed = continued(read)
        XCTAssertEqual(crashed.cause, .crashed("b"))
        XCTAssertEqual(crashed.order, ["c"])
        XCTAssertTrue(try XCTUnwrap(crashed.report.results.last).crashed)
        XCTAssertEqual(crashed.crash?.id, "b")

        let restarted = continued(read, boot: "B2")
        XCTAssertEqual(restarted.cause, .restarted("b"))
        XCTAssertEqual(restarted.order, ["c", "b"])
        XCTAssertEqual(restarted.report.resumptions?.map(\.bootUUID), ["B2"])
        XCTAssertEqual(restarted.report.results.map(\.id), ["a"])
    }

    func testARunsSpanRunsFromItsStartAcrossARelaunchAndARestart() throws {
        let going = continued(planned(), boot: "B2")
        XCTAssertEqual(going.report.milliseconds(at: Date(timeIntervalSince1970: 2045.5)), 2_045_500)
    }

    func testARunWithNothingLeftEndsWhenContinued() throws {
        var report = planned()
        for id in ids { report.record(record(id, at: 1)) }
        let going = continued(report)
        XCTAssertEqual(going.order, [])
        XCTAssertEqual(going.report.run.status, .done)
    }

    func testAnInterruptionAfterTheRunClosedDoesNotBringItsJournalBack() throws {
        let journal = try store.open(planned())
        try journal.advance(nil, begin: "a")
        try store.close(journal.report)
        XCTAssertThrowsError(try journal.interrupt("backgrounded"))
        XCTAssertFalse(FileManager.default.fileExists(atPath: journal.url.path))
    }

    func testARerunsJournalIsReadOverTheFinishedJsonAndOlderBootsAreNotRead() throws {
        let older = planned(boot: "B0", started: 0)
        try store.close(older)
        let oldest = planned(boot: "B0", started: -60)
        try store.close(oldest)
        let finished = uninterrupted()
        try store.close(finished)
        var reopened = finished
        reopened.plan(["a"])
        let rerun = try store.open(reopened)
        try rerun.advance(nil, begin: "a")
        let files = try FileManager.default.contentsOfDirectory(atPath: store.directory.path).sorted()
        let age = [oldest.run.id: 30.0, older.run.id: 20.0, finished.run.id: 10.0]
        for name in files {
            let id = String(name.split(separator: ".")[0])
            let when = Date(timeIntervalSinceNow: -(age[id] ?? 0) + (name.hasSuffix(".journal") ? 5 : 0))
            try FileManager.default.setAttributes(
                [.modificationDate: when], ofItemAtPath: store.directory.appendingPathComponent(name).path
            )
        }

        let recent = store.recent(boot: boot)
        XCTAssertEqual(recent.map(\.run.id), [older.run.id, finished.run.id], "stops at the last run of boot B0")
        XCTAssertEqual(recent.last?.run.current, "a", "the journal, not the json")
        XCTAssertEqual(Report.interrupted(recent)?.run.id, finished.run.id)
        XCTAssertNil(Report.lastBootStoppedIn(recent, boot: boot), "B0's last run was between tests")
    }

    /// The same text as tools/hwtest/fetch_test.py's JOURNAL: both readers fold it to the same run.
    static let sharedJournal = """
    {"identity":{"build":"24B119"},"results":[],"run":{"boot_uuid":"B1","id":"B1-19700101-000000","ios":27,\
    "order":["a","b","c"],"started":"1970-01-01T00:00:00.000Z","status":"running"},"schema":2,\
    "summary":{"crashed":0,"fail":0,"pass":0,"planned":3,"skip":0}}
    {"begin":"a"}
    {"result":{"boot_uuid":"B1","detail":"","finished":"1970-01-01T00:00:01.000Z","id":"a","interactive":false,\
    "log":"HWTEST a PASS ms=3.0","metrics":{"fps":59.94},"milliseconds":3,"name":"a","proof":"","subsystem":"Storage",\
    "verdict":"PASS"}}
    {"begin":"b"}
    {"interruption":"backgrounded"}
    {"result":{"boot_uuid":"B1","detail":"bad","finished":"1970-01-01T00:00:02.000Z","id":"b","interactive":false,\
    "log":"HWTEST b FAIL ms=3.0 why=bad","metrics":{},"milliseconds":3,"name":"b","proof":"","subsystem":"Stor
    """

    func testTheSharedJournalFoldsToItsRun() throws {
        let read = try XCTUnwrap(RunJournal.report(from: Data(Self.sharedJournal.utf8)))
        XCTAssertEqual(read.run.current, "b")
        XCTAssertEqual(read.run.interruption, "backgrounded")
        XCTAssertEqual(read.run.status, .running)
        XCTAssertEqual(read.results.map(\.id), ["a"])
        XCTAssertEqual(read.summary, Report.Summary(pass: 1, fail: 0, skip: 0, crashed: 0, planned: 3))
        XCTAssertEqual(read.results[0].metrics["fps"], .number(59.94, digits: 2))
    }

    func testAJournalWhoseFirstLineIsNoReportReadsAsNone() {
        XCTAssertNil(RunJournal.report(from: Data()))
        XCTAssertNil(RunJournal.report(from: Data("{\"begin\":\"a\"}\n".utf8)))
    }
}
