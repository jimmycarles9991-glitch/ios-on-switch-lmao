import Foundation
import HWTestCore
import XCTest

final class ReportTests: XCTestCase {
    let boot = "6A25F6C4-A931-4851-9437-EDE961F57EC1"

    func record(_ id: String, _ outcome: Outcome, ms: Double = 12.34) -> TestRecord {
        TestRecord(
            id: id, subsystem: "video", name: id, interactive: false, outcome: outcome, milliseconds: ms,
            finished: Date(timeIntervalSince1970: 0)
        )
    }

    func report(started: TimeInterval = 0) -> Report {
        Report(bootUUID: boot, ios: 27, started: Date(timeIntervalSince1970: started), identity: ["build": "24B119"])
    }

    func testARunNamesItsBootAndStartAndSortsByThem() {
        let first = report(started: 0)
        let later = report(started: 3600)
        XCTAssertEqual(first.fileName, "\(boot)-19700101-000000.json")
        XCTAssertLessThan(first.run.id, later.run.id)
        XCTAssertEqual(first.run.started, "1970-01-01T00:00:00.000Z")
    }

    func testRunningTestIsCurrentUntilItsRecordLands() {
        var run = report()
        run.plan(["a", "b", "c"])
        run.begin("a")
        XCTAssertEqual(run.run.status, .running)
        XCTAssertEqual(run.run.current, "a")
        XCTAssertEqual(run.remaining, ["b", "c"])
        run.record(record("a", .pass()))
        XCTAssertNil(run.run.current)
        XCTAssertEqual(run.remaining, ["b", "c"])
        XCTAssertEqual(run.summary, Report.Summary(pass: 1, fail: 0, skip: 0, crashed: 0, planned: 3))
        run.finish(at: Date(timeIntervalSince1970: 10))
        XCTAssertEqual(run.run.status, .done)
        XCTAssertEqual(run.run.finished, "1970-01-01T00:00:10.000Z")
    }

    func testARerunReplacesItsEntryInPlace() {
        var run = report()
        run.plan(["a", "b"])
        run.record(record("a", .fail("first")))
        run.record(record("b", .pass()))
        run.record(record("a", .pass(), ms: 3))
        XCTAssertEqual(run.results.map(\.id), ["a", "b"])
        XCTAssertEqual(run.results[0].log, "HWTEST a PASS ms=3.0")
        XCTAssertEqual(run.summary.pass, 2)
    }

    func testOnlyTheNewestUnfinishedRunIsOffered() {
        XCTAssertNil(Report.interrupted([]))
        var older = report(started: 0)
        older.plan(["a"])
        var newer = report(started: 60)
        newer.plan(["a", "b"])
        newer.begin("a")
        XCTAssertEqual(Report.interrupted([older, newer])?.run.id, newer.run.id)
        newer.finish(at: Date())
        XCTAssertNil(Report.interrupted([older, newer]), "the newest run finished; an older one is stale")
        var anotherBoot = Report(bootUUID: "another boot", ios: 27, started: Date(), identity: [:])
        anotherBoot.plan(["c"])
        XCTAssertEqual(Report.interrupted([older, newer, anotherBoot])?.run.id, anotherBoot.run.id)
    }

    func testTheTestAnEarlierBootWentDownInIsNamedOnlyByThatBootsLastRun() {
        var stopped = Report(bootUUID: "B1", ios: 27, started: Date(timeIntervalSince1970: 0), identity: [:])
        stopped.plan(["a", "b"])
        stopped.begin("b")
        var mine = Report(bootUUID: "B2", ios: 27, started: Date(timeIntervalSince1970: 60), identity: [:])
        mine.plan(["a"])
        XCTAssertEqual(Report.lastBootStoppedIn([stopped, mine], boot: "B2"), "b")
        XCTAssertNil(Report.lastBootStoppedIn([stopped, mine], boot: "B1"), "B2's last run was between tests")
        var finished = Report(bootUUID: "B1", ios: 27, started: Date(timeIntervalSince1970: 30), identity: [:])
        finished.finish(at: Date())
        XCTAssertNil(Report.lastBootStoppedIn([stopped, finished], boot: "B2"))
    }

    func testAnInterruptedTestRunsAgainInsteadOfCountingAsACrash() throws {
        var run = report()
        run.plan(["a", "b", "c"])
        run.record(record("a", .pass()))
        run.begin("b")
        run.interrupt("backgrounded")
        let saved = try Report.decoded(try run.encoded())
        XCTAssertEqual(saved.run.interruption, "backgrounded")
        XCTAssertEqual(Report.interrupted([saved])?.run.current, "b")
        var resumed = saved
        resumed.requeueInterrupted()
        XCTAssertEqual(resumed.remaining, ["b", "c"])
        XCTAssertNil(resumed.run.interruption)
        XCTAssertEqual(resumed.summary.crashed, 0)
        run.interrupt(nil)
        XCTAssertNil(run.run.interruption, "back in the foreground")
    }

    func testACrashIsAFailThatSaysSo() throws {
        var run = report()
        run.plan(["a", "b"])
        run.begin("a")
        run.record(record("a", Report.crashOutcome("a")))
        XCTAssertTrue(run.results[0].crashed)
        XCTAssertEqual(run.summary.crashed, 1)
        XCTAssertEqual(run.summary.fail, 1)
        XCTAssertTrue(run.results[0].log.hasPrefix("HWTEST a FAIL crashed=1 ms=12.3 why=the_app's_process_ended"))
        XCTAssertTrue(try Report.decoded(try run.encoded()).results[0].crashed)
    }

    func testAnInterruptedRerunRetriesInsteadOfKeepingItsOlderResult() {
        var run = report()
        run.plan(["a", "b"])
        run.record(record("a", .pass()))
        run.record(record("b", .pass()))
        run.begin("a")
        run.interrupt("closed")
        run.requeueInterrupted()
        XCTAssertEqual(run.remaining, ["a"])
        XCTAssertEqual(run.results.map(\.id), ["b"])
        XCTAssertEqual(run.summary.pass, 1)
    }

    func testCancellationKeepsPartialResultsAndDoesNotOfferThemAgain() throws {
        var run = report()
        run.plan(["a", "b"])
        run.record(record("a", .pass()))
        run.begin("b")
        run.interrupt("closed")
        let results = run.results
        run.cancel(at: Date(timeIntervalSince1970: 60))
        let saved = try Report.decoded(try run.encoded())
        XCTAssertEqual(saved.run.status, .cancelled)
        XCTAssertEqual(saved.results, results)
        XCTAssertEqual(saved.run.order, ["a", "b"])
        XCTAssertEqual(saved.run.finished, "1970-01-01T00:01:00.000Z")
        XCTAssertNil(saved.run.current)
        XCTAssertNil(saved.run.interruption)
        var older = report(started: -60)
        older.plan(["a"])
        XCTAssertNil(Report.interrupted([older, saved]))
    }

    func testResumingOnAnotherBootPreservesOriginAndAttributesNewResults() throws {
        var run = report()
        run.plan(["a", "b", "c"])
        run.record(record("a", .pass()))
        run.begin("b")
        let origin = run.run
        let results = run.results
        let resumedIdentity = ["build": "24B119", "app_uuid": "new app"]
        run.resume(bootUUID: "B2", at: Date(timeIntervalSince1970: 60), identity: resumedIdentity)
        XCTAssertEqual(run.run, origin)
        XCTAssertEqual(run.identity, ["build": "24B119"])
        XCTAssertEqual(run.results, results)
        XCTAssertEqual(run.latestBootUUID, "B2")
        XCTAssertEqual(run.resumptions?.last?.identity, resumedIdentity)
        XCTAssertEqual(run.resumptions?.last?.at, "1970-01-01T00:01:00.000Z")
        XCTAssertNil(Report.lastBootStoppedIn([run], boot: "B2"))
        XCTAssertEqual(Report.lastBootStoppedIn([run], boot: "B3"), "b")
        run.resume(bootUUID: "B2", at: Date(), identity: resumedIdentity)
        XCTAssertEqual(run.resumptions?.count, 1)
        run.requeueInterrupted()
        run.record(record("b", .pass()))
        XCTAssertEqual(run.bootUUID(for: run.results[0]), boot)
        XCTAssertEqual(run.bootUUID(for: run.results[1]), "B2")
        run.record(record("a", .pass()))
        XCTAssertEqual(run.bootUUID(for: run.results[0]), "B2", "a rerun belongs to the boot that reran it")
        run.resume(bootUUID: "B3", at: Date(timeIntervalSince1970: 120), identity: ["build": "24B120"])
        XCTAssertEqual(run.resumptions?.map(\.bootUUID), ["B2", "B3"])
        XCTAssertEqual(run.latestBootUUID, "B3")
        XCTAssertEqual(run.bootUUID(for: run.results[0]), "B2")
        XCTAssertEqual(try Report.decoded(try run.encoded()), run)
    }

    func testOlderReportsWithoutResumeFieldsDecodeWithTheirOriginalBoot() throws {
        var run = report()
        run.plan(["a", "b"])
        run.record(record("a", .pass()))
        var json = try JSONSerialization.jsonObject(with: run.encoded()) as! [String: Any]
        json.removeValue(forKey: "resumptions")
        var entries = json["results"] as! [[String: Any]]
        entries[0].removeValue(forKey: "boot_uuid")
        json["results"] = entries
        let saved = try Report.decoded(JSONSerialization.data(withJSONObject: json))
        XCTAssertNil(saved.resumptions)
        XCTAssertNil(saved.results[0].bootUUID)
        XCTAssertEqual(saved.latestBootUUID, boot)
        XCTAssertEqual(saved.bootUUID(for: saved.results[0]), boot)
    }

    func testRoundTripsThroughJSONWithSnakeCaseRunKeys() throws {
        var metrics = Metrics()
        metrics.set("frames", 60)
        metrics.set("fps", 59.94)
        metrics.set("hardware", true)
        var run = report()
        run.plan(["v"])
        run.record(record("v", .pass(metrics)))
        let data = try run.encoded()
        XCTAssertEqual(try Report.decoded(data), run)
        let json = try JSONSerialization.jsonObject(with: data) as! [String: Any]
        XCTAssertEqual(json["schema"] as? Int, 2)
        XCTAssertEqual((json["run"] as! [String: Any])["boot_uuid"] as? String, boot)
        let encoded = (json["results"] as! [[String: Any]])[0]["metrics"] as! [String: Any]
        XCTAssertEqual(encoded["frames"] as? Int, 60)
        XCTAssertEqual(encoded["fps"] as? Double, 59.94)
        XCTAssertEqual(encoded["hardware"] as? Bool, true)
    }

    func testAnEntryReadBackKeepsItsVerdictAndMetrics() {
        var metrics = Metrics()
        metrics.set("z", 1)
        metrics.set("a", "x")
        let outcome = Outcome(Report.Entry(record("v", .fail("bad", metrics, proof: "hw"))))
        XCTAssertEqual(outcome.verdict, .fail)
        XCTAssertEqual(outcome.metrics.keys, ["a", "z"])
        XCTAssertEqual(outcome.detail, "bad")
        XCTAssertEqual(outcome.proof, "hw")
    }

    func testDurableWriteReplacesTheFileWhole() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = directory.appendingPathComponent("run.json")
        try DurableFile.write(Data("first".utf8), to: url)
        try DurableFile.write(Data("second".utf8), to: url)
        XCTAssertEqual(try String(contentsOf: url, encoding: .utf8), "second")
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: directory.path), ["run.json"])
    }
}
