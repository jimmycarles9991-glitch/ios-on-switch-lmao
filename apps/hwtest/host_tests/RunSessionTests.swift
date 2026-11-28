import Foundation
import HWTestCore
import XCTest

final class RunSessionTests: XCTestCase {
    func record(_ id: String, _ outcome: Outcome = .pass()) -> TestRecord {
        TestRecord(
            id: id, subsystem: "Storage", name: id, interactive: false, outcome: outcome, milliseconds: 5,
            finished: Date(timeIntervalSince1970: 10)
        )
    }

    func report(boot: String = "B1", started: TimeInterval = 0) -> Report {
        Report(bootUUID: boot, ios: 27, started: Date(timeIntervalSince1970: started), identity: ["build": "24B119"])
    }

    func unfinished(boot: String = "B1", started: TimeInterval = 0) -> Report {
        var run = report(boot: boot, started: started)
        run.plan(["a", "b"])
        run.begin("a")
        run.record(record("a"))
        run.begin("b")
        return run
    }

    func testLaunchWithNoPrecedingStateStaysEmpty() {
        var session = RunSession()
        XCTAssertEqual(session.applyLaunch(recent: [], boot: "B1"), .empty)
        XCTAssertNil(session.report)
        XCTAssertNil(session.pendingRun)
        XCTAssertFalse(session.resumePromptShown)
        XCTAssertFalse(session.reportSaved)

        var earlierDone = report(boot: "B0")
        earlierDone.plan(["a"])
        earlierDone.record(record("a"))
        earlierDone.finish(at: Date(timeIntervalSince1970: 20))
        XCTAssertEqual(session.applyLaunch(recent: [earlierDone], boot: "B1"), .empty)
        XCTAssertNil(session.report)
        XCTAssertNil(session.pendingRun)
        XCTAssertFalse(session.resumePromptShown)
    }

    func testLaunchShowsFinishedOrCancelledRunFromThisBoot() {
        var done = report(boot: "B1", started: 0)
        done.plan(["a"])
        done.record(record("a"))
        done.finish(at: Date(timeIntervalSince1970: 20))

        var session = RunSession()
        XCTAssertEqual(session.applyLaunch(recent: [done], boot: "B1"), .showLast(done))
        XCTAssertEqual(session.report, done)
        XCTAssertTrue(session.reportSaved)
        XCTAssertNil(session.pendingRun)
        XCTAssertFalse(session.resumePromptShown)

        var cancelled = unfinished(boot: "B1", started: 60)
        cancelled.cancel(at: Date(timeIntervalSince1970: 90))
        XCTAssertEqual(session.applyLaunch(recent: [done, cancelled], boot: "B1"), .showLast(cancelled))
        XCTAssertEqual(session.report, cancelled)
        XCTAssertTrue(session.reportSaved)
    }

    func testLaunchPromptsForUnfinishedRunFromSameOrEarlierBoot() {
        let sameBoot = unfinished(boot: "B1")
        var session = RunSession()
        XCTAssertEqual(session.applyLaunch(recent: [sameBoot], boot: "B1"), .promptUnfinished(sameBoot))
        XCTAssertEqual(session.pendingRun, sameBoot)
        XCTAssertTrue(session.resumePromptShown)
        XCTAssertNil(session.report)
        XCTAssertFalse(session.reportSaved)

        let earlierBoot = unfinished(boot: "B0")
        XCTAssertEqual(session.applyLaunch(recent: [earlierBoot], boot: "B1"), .promptUnfinished(earlierBoot))
        XCTAssertEqual(session.pendingRun, earlierBoot)
        XCTAssertTrue(session.resumePromptShown)
    }

    func testDismissingPromptKeepsPendingRunAndAllowsEphemeralAdHocRuns() {
        let pending = unfinished(boot: "B1", started: 0)
        var session = RunSession()
        _ = session.applyLaunch(recent: [pending], boot: "B1")
        session.dismissResumePrompt()
        XCTAssertFalse(session.resumePromptShown)
        XCTAssertEqual(session.pendingRun, pending)
        XCTAssertNil(session.report)

        let first = session.planBatch(["audio.headset"]) { self.report(boot: "B1", started: 100) }
        XCTAssertFalse(first.persists)
        XCTAssertFalse(session.reportSaved)
        XCTAssertEqual(first.report.run.order, ["audio.headset"])
        XCTAssertNotEqual(first.report.run.id, pending.run.id)
        XCTAssertEqual(session.pendingRun, pending)

        var finishedFirst = first.report
        finishedFirst.record(record("audio.headset"))
        finishedFirst.finish(at: Date(timeIntervalSince1970: 110))
        session.finishBatch(finishedFirst)
        XCTAssertEqual(session.report, finishedFirst)
        XCTAssertFalse(session.reportSaved)

        let second = session.planBatch(["gpu.clear.formats"]) { self.report(boot: "B1", started: 200) }
        XCTAssertFalse(second.persists)
        XCTAssertEqual(second.report.run.id, finishedFirst.run.id)
        XCTAssertEqual(second.report.run.order, ["audio.headset", "gpu.clear.formats"])

        XCTAssertEqual(session.requestRunAll(), .promptUnfinished)
        XCTAssertTrue(session.resumePromptShown)
        session.dismissResumePrompt()
        XCTAssertFalse(session.resumePromptShown)
        XCTAssertEqual(session.pendingRun, pending)
        XCTAssertEqual(session.report?.run.id, finishedFirst.run.id)

        let continued = pending.continued(
            boot: "B1", at: Date(timeIntervalSince1970: 300), identity: [:], available: ["a", "b"]
        ) { id, outcome in self.record(id, outcome) }
        session.acceptPendingChoice(continued.report)
        XCTAssertNil(session.pendingRun)
        XCTAssertFalse(session.resumePromptShown)
        XCTAssertEqual(session.report, continued.report)
        XCTAssertTrue(session.reportSaved)
    }

    func testFreshStartAndAdHocBatchesWithoutPendingRunPersist() {
        var session = RunSession()
        _ = session.applyLaunch(recent: [], boot: "B1")
        XCTAssertEqual(session.requestRunAll(), .startFresh)

        let adHoc = session.planBatch(["audio.headset"]) { self.report(boot: "B1", started: 10) }
        XCTAssertTrue(adHoc.persists)
        XCTAssertTrue(session.reportSaved)
        XCTAssertEqual(adHoc.report.run.order, ["audio.headset"])

        let fresh = report(boot: "B1", started: 60)
        session.beginFreshRun(fresh)
        let full = session.planBatch(["a", "b"]) { self.report(boot: "B1", started: 70) }
        XCTAssertTrue(full.persists)
        XCTAssertEqual(full.report.run.id, fresh.run.id)
        XCTAssertEqual(full.report.run.order, ["a", "b"])
    }

    func testCancelAndAdHocRunLeavesPendingJournalIntactInStore() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = RunStore(directory: directory)

        var initial = report(boot: "B1", started: 0)
        initial.plan(["a", "b"])
        let journal = try store.open(initial)
        try journal.advance(nil, begin: "a")
        try journal.advance(record("a"), begin: "b")
        let saved = journal.report

        var session = RunSession()
        XCTAssertEqual(session.applyLaunch(recent: store.recent(boot: "B1"), boot: "B1"), .promptUnfinished(saved))
        session.dismissResumePrompt()
        let adHoc = session.planBatch(["audio.headset"]) { self.report(boot: "B1", started: 50) }
        XCTAssertFalse(adHoc.persists)

        let reloaded = store.recent(boot: "B1")
        XCTAssertEqual(reloaded, [saved])
        XCTAssertEqual(Report.interrupted(reloaded), saved)
    }
}
