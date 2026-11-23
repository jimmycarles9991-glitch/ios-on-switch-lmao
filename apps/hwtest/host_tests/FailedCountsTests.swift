import Foundation
import HWTestCore
import XCTest

/// The facade's failure and skip counters as a test judges them, whose each failure was, and Run All's roll-up.
final class FailedCountsTests: XCTestCase {
    func paths(_ paths: [String: Int]) -> PathTally {
        var tally = PathTally()
        for (path, count) in paths { tally.add(path, count) }
        return tally
    }

    func record(_ id: String, _ verdict: Verdict, _ fields: [String: String]) -> TestRecord {
        var metrics = Metrics()
        for key in fields.keys.sorted() { metrics.set(key, fields[key]!) }
        return TestRecord(
            id: id, subsystem: "metal", name: id, interactive: false, outcome: Outcome(verdict, metrics),
            milliseconds: 1, finished: Date(timeIntervalSince1970: 0)
        )
    }

    func testCountsReadFailuresAndSkipsBetweenTwoReads() {
        let before = ["failed": 2, "failed.call.buf": 2, "skipped": 1, "skipped.call.blit": 1]
        let counts = FailedCounts(
            before: before,
            after: ["failed": 5, "failed.call.buf": 3, "failed.none": 2, "skipped": 4, "skipped.call.blit": 4]
        )
        XCTAssertEqual(counts.metrics["failed_cbs"], .integer(3))
        XCTAssertEqual(counts.metrics["failed_why"], .text("none/2,call.buf/1"))
        XCTAssertEqual(counts.metrics["skipped_calls"], .integer(3))
        XCTAssertEqual(counts.metrics["skipped_why"], .text("call.blit/3"))
        XCTAssertNil(FailedCounts(before: before, after: before).metrics["skipped_why"])
        let unanswered = FailedCounts(before: [:], after: ["gm20b": 4])
        XCTAssertEqual(unanswered.metrics["failed_cbs"], .text("unanswered"))
        XCTAssertEqual(
            unanswered.check(own: paths(["gm20b": 4])).0,
            "no command buffer failed: the facade's counters answer no failed count"
        )
        XCTAssertFalse(unanswered.skipCheck().1)
        XCTAssertEqual(FailedCounts.counts("none/2,call.buf/1"), ["none": 2, "call.buf": 1])
    }

    func testAFailureOfTheTestsOwnCommandBufferIsItsOwn() {
        let counts = FailedCounts(before: ["failed": 0], after: ["failed": 1, "failed.enc.pass": 1])
        let attribution = counts.attributed(own: paths(["gm20b": 3, "error:enc.pass": 1]))
        XCTAssertFalse(attribution.holds)
        XCTAssertEqual(attribution.check.0, "own failure: its own command buffers failed enc.pass/1")
        XCTAssertEqual(attribution.metrics["own_failed"], .integer(1))
        XCTAssertEqual(attribution.metrics["own_why"], .text("enc.pass/1"))
        XCTAssertNil(attribution.metrics["foreign_failed"])
    }

    func testAFailureOfAnotherCommandBufferIsForeignWithItsOwnKey() {
        let counts = FailedCounts(before: ["failed": 0], after: ["failed": 2, "failed.none": 2])
        let attribution = counts.attributed(own: paths(["gm20b": 14]))
        XCTAssertEqual(
            attribution.check.0,
            "foreign failure: foreign_failed=2 none/2 in command buffers it did not commit (a framework's, another "
                + "queue's or thread's); its own 14 ended as expected"
        )
        XCTAssertEqual(attribution.metrics["foreign_failed"], .integer(2))
        XCTAssertEqual(attribution.metrics["foreign_why"], .text("none/2"))
        XCTAssertNil(attribution.metrics["own_why"])
        XCTAssertTrue(counts.check(own: PathTally()).0.hasSuffix("; it committed none itself"))
        let both = FailedCounts(before: [:], after: ["failed": 3, "failed.none": 2, "failed.enc.pass": 1])
        XCTAssertEqual(
            both.check(own: paths(["error:enc.pass": 1])).0,
            "own failure: its own command buffers failed enc.pass/1; foreign failure: foreign_failed=2 none/2 in "
                + "command buffers it did not commit (a framework's, another queue's or thread's)"
        )
    }

    func testExpectedFailuresAreTheTestsOwnAndExact() {
        let one = FailedCounts(before: [:], after: ["failed": 1, "failed.compute": 1])
        let own = one.check(expecting: ["compute": 1], own: paths(["error:compute": 1]))
        XCTAssertEqual(own.0, "exactly compute/1 failed")
        XCTAssertTrue(own.1)
        let two = FailedCounts(before: [:], after: ["failed": 2, "failed.compute": 2])
        XCTAssertEqual(
            two.check(expecting: ["compute": 1], own: paths(["error:compute": 2])).0,
            "own failure: its own command buffers failed compute/1 past the expected compute/1"
        )
        XCTAssertTrue(
            two.check(expecting: ["compute": 1], own: paths(["error:compute": 1])).0.hasPrefix(
                "foreign failure: foreign_failed=1 compute/1"
            )
        )
        let none = FailedCounts(before: [:], after: ["failed": 0])
        XCTAssertEqual(
            none.check(expecting: ["compute": 1], own: paths(["gm20b": 1])).0,
            "own failure: expected compute/1 more to fail (failed -)"
        )
        XCTAssertEqual(
            none.check(own: paths(["gm20b": 1, "pending": 1])).0,
            "own failure: its own command buffers off the driver pending/1"
        )
        XCTAssertEqual(
            none.check(own: paths(["error:pass.fmt": 1])).0,
            "own failure: its own command buffers failed pass.fmt/1; own failure: its own paths failed pass.fmt/1 the "
                + "counters did not count"
        )
    }

    func testSkipsAreJudgedExactlyAndAMissingCountFails() {
        let counts = FailedCounts(before: ["skipped": 0], after: ["skipped": 1, "skipped.call.blit": 1])
        XCTAssertTrue(counts.skipCheck(expecting: ["call.blit": 1]).1)
        XCTAssertEqual(counts.skipCheck().0, "no call was skipped, not call.blit/1 (skipped_calls=1)")
        XCTAssertTrue(FailedCounts(before: ["skipped": 3], after: ["skipped": 3]).skipCheck().1)
        var metrics = Metrics()
        let checks = FailedCounts.judgedFramework(before: [:], after: ["failed": 0], own: PathTally(), &metrics)
        XCTAssertEqual(checks.map(\.1), [true, false])
        XCTAssertEqual(checks[1].0, "no call was skipped: the facade's counters answer no skipped count")
    }

    func testAnArmByArmTestSumsItsCauses() {
        let a = FailedCounts(before: [:], after: ["failed": 1, "failed.none": 1]).attributed(own: PathTally())
        let b = FailedCounts(before: [:], after: ["failed": 2, "failed.none": 1, "failed.enc.pass": 1])
            .attributed(own: paths(["error:enc.pass": 1]))
        let metrics = FailedAttribution.metrics([a, b])
        XCTAssertEqual(metrics["foreign_why"], .text("none/2"))
        XCTAssertEqual(metrics["own_why"], .text("enc.pass/1"))
    }

    func testRunAllRollsUpTheFirstRedOfEachPredicateAndItsTests() {
        let records = [
            record("a", .pass, ["failed_why": "compute/1"]),
            record("b", .fail, ["own_why": "enc.pass/1"]),
            record("c", .fail, ["foreign_why": "none/2,enc.pass/1"]),
            record("d", .fail, ["own_why": "none/1"]),
            record("e", .pass, ["skipped_why": "call.blit/5"]),
            record("f", .fail, ["skipped_why": "call.blit/1,call.icb/1"]),
            record("g", .pass, ["own_why": "none/1"]),
        ]
        XCTAssertEqual(
            LogLine.rollUp(records, milliseconds: 5),
            "HWTEST suite.reds FAIL failed_reds=enc.pass/2@b:own,none/2@c:foreign "
                + "skipped_tests=call.blit/2@e,call.icb/1@f ms=5.0"
        )
        XCTAssertEqual(
            LogLine.rollUp(Array(records.prefix(1)), milliseconds: 5),
            "HWTEST suite.reds PASS failed_reds=- skipped_tests=- ms=5.0"
        )
    }

    func testACutLineKeepsTheCauseOfItsRed() {
        var metrics = Metrics()
        metrics.set("own_why", "enc.pass/1")
        metrics.set("detail", String(repeating: "x", count: 300))
        metrics.fitLog(within: 40, keeping: [])
        XCTAssertEqual(metrics.reportOnly, ["detail"])
    }

    func testATallyGrownFromAnotherSaysWhatWasAdded() {
        let earlier = paths(["gm20b": 2, "error:none": 1])
        let later = paths(["gm20b": 5, "error:none": 1, "pending": 1])
        XCTAssertEqual(later.since(earlier), paths(["gm20b": 3, "pending": 1]))
    }
}
