import HWTestCore
import XCTest

/// Review of 10a5cc189: at Run All's 8 trials a cell a barrier failing 31% of the time could read clean
final class ProbeQuickTests: XCTestCase {
    func testAQuickCleanClaimIsNotProof() {
        var finding = ProbeFinding(.pass)
        finding.quick(unless: false)
        XCTAssertEqual(finding.verdict, .passQuick)
        XCTAssertEqual(
            finding.outcome(probe: "p", base: Metrics(), proof: "").metrics["claim_verdict"],
            .text("PASS_QUICK")
        )
    }

    func testLongRunsKeepPassAndAFailStaysAFail() {
        var long = ProbeFinding(.pass)
        long.quick(unless: true)
        XCTAssertEqual(long.verdict, .pass)
        for verdict in [ClaimVerdict.fail, .inconclusive] {
            var finding = ProbeFinding(verdict)
            finding.quick(unless: false)
            XCTAssertEqual(finding.verdict, verdict)
        }
    }
}
