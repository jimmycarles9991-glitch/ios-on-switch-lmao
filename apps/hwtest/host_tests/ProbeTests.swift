import Foundation
import HWTestCore
import XCTest

final class ProbeTests: XCTestCase {
    func line(_ finding: ProbeFinding) -> String {
        var base = Metrics()
        base.set("paths", "raw/1")
        let outcome = finding.outcome(probe: "d23", base: base, proof: "GM20B")
        return LogLine.format(id: "gpu.x1.rop_conversion", outcome: outcome, milliseconds: 1)
    }

    func testARunProbeIsAPassingLineCarryingTheClaimsVerdict() {
        let finding = ProbeFinding.counted(["unorm16_wrong": 3, "srgb8_wrong": 0], paths: ["raw"])
        XCTAssertEqual(finding.verdict, .fail)
        XCTAssertEqual(
            line(finding),
            "HWTEST gpu.x1.rop_conversion PASS probe=d23 claim_verdict=FAIL paths=raw/1 srgb8_wrong=0 unorm16_wrong=3 "
                + "hw=GM20B ms=1.0 why=wrong:_unorm16_wrong")
    }

    func testLongFieldsGoToTheReportAndTheLineKeepsTheVerdicts() {
        var metrics = Metrics()
        metrics.set("rule", "copy.twod")
        metrics.set("cells", String(repeating: "bgra8_1to1_point:exact:0,", count: 80))
        metrics.set("exact_cells", 80)
        let outcome = ProbeFinding(.pass, metrics).outcome(probe: "copy.twod", base: Metrics(), proof: "GM20B")
        let text = LogLine.format(id: "gpu.x1.twod_scale", outcome: outcome, milliseconds: 1)
        XCTAssertLessThan(text.count, 1000)
        XCTAssertEqual(
            text,
            "HWTEST gpu.x1.twod_scale PASS probe=copy.twod claim_verdict=PASS rule=copy.twod exact_cells=80 "
                + "report_only=1 hw=GM20B ms=1.0")
        XCTAssertEqual(outcome.metrics["cells"], .text(String(repeating: "bgra8_1to1_point:exact:0,", count: 80)))
    }

    func testNoWrongCountPasses() {
        XCTAssertEqual(ProbeFinding.counted(["a_wrong": 0], paths: ["raw", "raw"]).verdict, .pass)
    }

    func testWorkOffTheRawPathDecidesNothing() {
        let finding = ProbeFinding.counted(["a_wrong": 0], paths: ["raw", "m2v:pipeline:compile"])
        XCTAssertEqual(finding.verdict, .inconclusive)
        XCTAssertEqual(finding.note, "not on the raw path: m2v:pipeline:compile")
    }
}
