import Foundation
import HWTestCore
import XCTest

final class DfllLandingTests: XCTestCase {
    /// debug.switch_perf_dfll_landing on boot 22 (13:46:20). The kernel's own log copy of it was cut at 1 KiB, so case
    /// 4's 200 ms registers and case 5 are filled in from the HWTEST line's 1785 MHz figures (1778.5 and 1779.8 MHz)
    /// with case 4's other readings' registers.
    static let boot22 = [
        "dfll=ran restore=1785,0",
        "0=1683>1581 0.2=1589772,1040ffbe,12207ff,4e200000,1007d,1c 0.10=1588751,1040ffbe,12207ff,4e200000,1007d,1c",
        "0.50=1586751,1040ffbe,12207ff,4e200000,1007d,1c 0.200=1586751,1040ffbe,12207ff,4e200000,1007d,1c",
        "1=1428>1581 1.2=1545824,1040ffbe,12207ff,4e200000,10079,1c 1.10=1587750,1040ffbe,12207ff,4e200000,1007d,1c",
        "1.50=1586751,1040ffbe,12207ff,4e200000,1007d,1c 1.200=1580780,1040ffbe,12207ff,4e200000,1007d,1c",
        "2=1785>1683 2.2=1693546,1340ddcc,3fff7ff,4e201000,10099,24 2.10=1674378,1340ddcc,3fff7ff,4e201000,10098,22",
        "2.50=1691251,1340ddcc,3fff7ff,4e201000,10099,24 2.200=1673266,1340ddcc,3fff7ff,4e201000,10098,22",
        "3=1581>1683 3.2=1681099,1340ddcc,12207ff,4e201000,10098,30 3.10=1676605,1340ddcc,3fff7ff,4e201000,10098,22",
        "3.50=1673266,1340ddcc,3fff7ff,4e201000,10097,23 3.200=1673266,1340ddcc,3fff7ff,4e201000,10098,22",
        "4=1683>1785 4.2=1797575,1340ebcc,3fff7ff,4e201000,10099,24 4.10=1779781,1340ebcc,3fff7ff,4e201000,10097,22",
        "4.50=1779781,1340ebcc,3fff7ff,4e201000,10098,22 4.200=1778525,1340ebcc,3fff7ff,4e201000,10098,22",
        "5=918>1785 5.2=1797575,1340ebcc,3fff7ff,4e201000,10099,24 5.10=1779781,1340ebcc,3fff7ff,4e201000,10097,22",
        "5.50=1779781,1340ebcc,3fff7ff,4e201000,10098,22 5.200=1779781,1340ebcc,3fff7ff,4e201000,10098,22",
    ].joined(separator: " ")

    /// The 200 ms readings of boot 2's run (00:14:41.779, its HWTEST line): 1581 MHz from below and 1785 MHz from 918
    /// 2 to 3 % off the case beside each, at the same loop registers
    static let boot2 = [
        "dfll=ran restore=1785,0",
        "0=1683>1581 0.200=1587750,1040ffbe,12207ff,4e200000,1007d,1c",
        "1=1428>1581 1.200=1543930,1040ffbe,12207ff,4e200000,1007d,1c",
        "2=1785>1683 2.200=1678860,1340e0cb,3fff7ff,4e201000,10094,20",
        "3=1581>1683 3.200=1696966,1340e0cb,3fff7ff,4e201000,10097,22",
        "4=1683>1785 4.200=1764804,1340eecb,3fff7ff,4e201000,10094,21",
        "5=918>1785 5.200=1801429,1340eecb,3fff7ff,4e201000,10094,20",
    ].joined(separator: " ")

    func judged(_ line: String) -> (outcome: Outcome, metrics: Metrics) {
        let (checks, metrics) = DfllLanding.judge(KernelLine.fields(line))
        return (Outcome.judged(checks, metrics).withHarnessFaults, metrics)
    }

    func judged(landings lines: [String]) -> (outcome: Outcome, metrics: Metrics) {
        let (checks, metrics) = DfllLanding.judge(landings: lines.map(KernelLine.fields))
        return (Outcome.judged(checks, metrics).withHarnessFaults, metrics)
    }

    func testBoot22sKeysAreLoggableAndItsLineFitsOsLog() {
        let (outcome, metrics) = judged(Self.boot22)
        XCTAssertEqual(metrics.faults, [])
        XCTAssertEqual(metrics["c0"], .text("1683_to_1581"))
        let line = LogLine.format(id: "cpu.dfll_landing", outcome: outcome, milliseconds: 2428.2)
        XCTAssertLessThanOrEqual(line.utf8.count, 1000, line)
        XCTAssertTrue(line.contains(" c0_200=1586751,1040ffbe,12207ff,4e200000,1007d,1c "), line)
    }

    func testBoot22LandsEveryRowWithinWhatTheDfllResolves() {
        let (outcome, _) = judged(Self.boot22)
        XCTAssertEqual(outcome.verdict, .pass, outcome.detail)
    }

    func testAPinnedRowStillAnswersToAQuarterPercent() {
        let pinned = Self.boot22.replacingOccurrences(
            of: "0.200=1586751,1040ffbe,12207ff,4e200000,1007d,1c",
            with: "0.200=1586751,1040ffbe,12207ff,4e200000,1007d,0"
        )
        let (outcome, _) = judged(pinned)
        XCTAssertEqual(outcome.verdict, .fail)
        XCTAssertTrue(outcome.detail.contains("1581 MHz at most 0.25% over"), outcome.detail)
    }

    func testARegulatingRowOverOneOutputStepFails() {
        let fast = Self.boot22.replacingOccurrences(
            of: "0.200=1586751,1040ffbe,12207ff,4e200000,1007d,1c",
            with: "0.200=1612000,1040ffbe,12207ff,4e200000,1007f,1c"
        )
        let (outcome, _) = judged(fast)
        XCTAssertEqual(outcome.verdict, .fail)
        XCTAssertTrue(outcome.detail.contains("1581 MHz at most 1.90% over"), outcome.detail)
    }

    func testAReadingOnTheUpperOfTwoOutputsTheLoopAlternatesPasses() {
        let upper = Self.boot22.replacingOccurrences(
            of: "4.200=1778525,1340ebcc,3fff7ff,4e201000,10098,22",
            with: "4.200=1797575,1340ebcc,3fff7ff,4e201000,10099,24"
        )
        let (outcome, metrics) = judged(upper)
        XCTAssertEqual(outcome.verdict, .pass, outcome.detail)
        XCTAssertEqual(metrics["over_pct_1785"]?.logText, "0.70")
    }

    func testBoot2sMissesAreTheProbeAwayFromTheLoopsOwnMonitor() {
        let (_, boot2) = judged(Self.boot2)
        XCTAssertEqual(boot2["under_pct_1581"]?.logText, "2.34")
        XCTAssertEqual(boot2["probe_gap_pct_1581"]?.logText, "3.13")
        XCTAssertEqual(boot2["probe_gap_pct_1785"]?.logText, "2.26")
        let (_, boot22) = judged(Self.boot22)
        XCTAssertEqual(boot22["probe_gap_pct_1581"]?.logText, "0.81")
        XCTAssertEqual(boot22["probe_gap_pct_1683"]?.logText, "0.44")
    }

    func testEveryLandingIsJudgedSoOneMissInFiveFailsTheRun() {
        let lines = (1 ... DfllLanding.landings).map { k in
            "soc_mc=4\(k)000,47000 " + (k == 3 ? Self.boot2 : Self.boot22)
        }
        let (outcome, metrics) = judged(landings: lines)
        XCTAssertEqual(outcome.verdict, .fail)
        XCTAssertTrue(outcome.detail.contains("landing 3: 1581 MHz at most 0.8% under"), outcome.detail)
        XCTAssertFalse(outcome.detail.contains("landing 1:"), outcome.detail)
        XCTAssertEqual(metrics["landings_failed"]?.logText, "1")
        XCTAssertEqual(metrics["l3_soc_mc"]?.logText, "43000,47000")
        XCTAssertEqual(metrics["l3_c1_200"]?.logText, "1543930,1040ffbe,12207ff,4e200000,1007d,1c")
        XCTAssertEqual(judged(landings: Array(repeating: Self.boot22, count: 5)).outcome.verdict, .pass)
    }

    func testFiveLandingsLogEachReadingAndTheCaseNamesOnce() {
        let (outcome, metrics) = judged(landings: Array(repeating: Self.boot22, count: DfllLanding.landings))
        XCTAssertEqual(metrics.faults, [])
        let line = LogLine.format(id: "cpu.dfll_landing", outcome: outcome, milliseconds: 12141.0)
        XCTAssertTrue(line.contains(" l1_c0=1683_to_1581 "), line)
        XCTAssertFalse(line.contains(" l2_c0="), line)
        XCTAssertTrue(line.contains(" l5_c5_200=1779781,1340ebcc,3fff7ff,4e201000,10098,22 "), line)
        XCTAssertTrue(line.contains(" l5_soc_mc=- "), line)
    }
}
