import Foundation
import HWTestCore
import XCTest

final class LogLineTests: XCTestCase {
    func record(_ id: String, _ outcome: Outcome, ms: Double = 12.34) -> TestRecord {
        TestRecord(
            id: id, subsystem: "video", name: id, interactive: false, outcome: outcome, milliseconds: ms,
            finished: Date(timeIntervalSince1970: 0)
        )
    }

    func testFormatOrdersMetricsThenProofDurationAndReason() {
        var metrics = Metrics()
        metrics.set("fps", 59.9412, digits: 1)
        metrics.set("frames", 60)
        metrics.set("hardware", true)
        let outcome = Outcome.fail("frame 3 has CRC\tmismatch", metrics, proof: "UsingHardware Decoder")
        XCTAssertEqual(
            LogLine.format(record("video.decode.h264_1080p", outcome)),
            "HWTEST video.decode.h264_1080p FAIL fps=59.9 frames=60 hardware=1 hw=UsingHardware_Decoder ms=12.3 "
                + "why=frame_3_has_CRC_mismatch"
        )
    }

    func testPassWithoutMetricsIsStillParseable() {
        let line = LogLine.format(record("cpu.single", .pass()))
        XCTAssertEqual(line, "HWTEST cpu.single PASS ms=12.3")
        XCTAssertEqual(LogLine.parse(line), LogLine.Parsed(id: "cpu.single", verdict: .pass, fields: ["ms": "12.3"]))
    }

    func testParseRoundTripsEveryField() {
        var metrics = Metrics()
        metrics.set("mbps", 812.5)
        metrics.set("name", "Tegra X1 (GM20B)")
        let line = LogLine.format(record("storage.seq", .pass(metrics, proof: "F_NOCACHE")))
        let parsed = LogLine.parse("Nov  7 12:00:00 SwitchHWTest[42] <Notice>: " + line)
        XCTAssertEqual(parsed?.verdict, .pass)
        XCTAssertEqual(parsed?.fields["mbps"], "812.50")
        XCTAssertEqual(parsed?.fields["name"], "Tegra_X1_(GM20B)")
        XCTAssertEqual(parsed?.fields["hw"], "F_NOCACHE")
    }

    func testRejectsMalformedLines() {
        XCTAssertNil(LogLine.parse("HWTEST video.decode MAYBE ms=1"))
        XCTAssertNil(LogLine.parse("HWTEST Video PASS ms=1"))
        XCTAssertNil(LogLine.parse("HWTEST video PASS novalue"))
        XCTAssertNil(LogLine.parse("nothing here"))
    }

    func testNonFiniteNumbersStayOneToken() {
        var metrics = Metrics()
        metrics.set("ratio", Double.infinity)
        metrics.set("other", Double.nan)
        XCTAssertEqual(LogLine.format(record("x.y", .pass(metrics))), "HWTEST x.y PASS ratio=inf other=nan ms=12.3")
    }

    func testSummaryFailsWhenAnyTestFailed() {
        let records = [record("a", .pass()), record("b", .skip("no headset")), record("c", .fail("bad"))]
        XCTAssertEqual(
            LogLine.summary(records, ios: 17, milliseconds: 1000),
            "HWTEST suite FAIL pass=1 fail=1 skip=1 ios=17 ms=1000.0")
        XCTAssertEqual(
            LogLine.summary(Array(records.prefix(2)), ios: 27, milliseconds: 5),
            "HWTEST suite PASS pass=1 fail=0 skip=1 ios=27 ms=5.0")
    }

    func testJudgedNamesTheFailedChecks() {
        let outcome = Outcome.judged([("fps >= 30", true), ("crc", false), ("hw", false)], Metrics())
        XCTAssertEqual(outcome.verdict, .fail)
        XCTAssertEqual(outcome.detail, "crc; hw")
        XCTAssertEqual(Outcome.judged([("ok", true)], Metrics()).verdict, .pass)
    }

    func testAReservedOrMalformedKeyIsAHarnessFailNotATrap() {
        var metrics = Metrics()
        metrics.set("ms", 1.5)
        metrics.set("Bad-Key", 2)
        metrics.set("fps", 60)
        var nested = Metrics()
        nested.set("why", "x")
        metrics.merge(nested, prefix: "")
        XCTAssertEqual(metrics.keys, ["fps"])
        XCTAssertEqual(metrics.faults, ["ms", "Bad-Key", "why"])
        let outcome = Outcome.pass(metrics).withHarnessFaults
        XCTAssertEqual(outcome.verdict, .fail)
        XCTAssertEqual(outcome.detail, "harness: bad metric key ms,Bad-Key,why")
        XCTAssertEqual(
            LogLine.format(record("image.png", outcome)),
            "HWTEST image.png FAIL fps=60 ms=12.3 why=harness:_bad_metric_key_ms,Bad-Key,why")
        let failed = Outcome.fail("no decoder", metrics).withHarnessFaults
        XCTAssertEqual(failed.detail, "harness: bad metric key ms,Bad-Key,why; no decoder")
        XCTAssertEqual(Outcome.pass().withHarnessFaults.verdict, .pass)
    }
}
