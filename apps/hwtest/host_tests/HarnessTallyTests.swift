import Foundation
import HWTestCore
import XCTest

final class HarnessTallyTests: XCTestCase {
    func tally(boundary: Double, mainWait: Double) -> HarnessTally {
        var tally = HarnessTally()
        for _ in 0 ..< 10 { tally.add(boundaryMs: boundary, flushMs: 30, mainWaitMs: mainWait) }
        tally.add(boundaryMs: 5000, flushMs: 455, mainWaitMs: 400)
        return tally
    }

    func testTheSuiteBetweenTestsPassesUnderItsCeilingsAndFailsOverThem() {
        let quick = tally(boundary: 45, mainWait: 0.2)
        XCTAssertEqual(quick.outcome.verdict, .pass, "one slow boundary is not the median")
        XCTAssertEqual(quick.outcome.metrics["boundaries"], .integer(11))
        XCTAssertEqual(quick.outcome.metrics["main_late"], .integer(1))
        XCTAssertTrue(quick.line.hasPrefix("HWTEST suite.harness PASS boundaries=11 boundary_ms_p50=45.0 "), quick.line)
        XCTAssertTrue(quick.line.hasSuffix(" ms=5450.0"), quick.line)
        XCTAssertEqual(tally(boundary: 565, mainWait: 0.2).outcome.verdict, .fail, "boot 2's harness")
        XCTAssertEqual(tally(boundary: 45, mainWait: 350).outcome.verdict, .fail, "a list redraw at every start")
    }

    func testTheLineCarriesWhatTheThreadSamplesCost() {
        var tally = HarnessTally()
        for ms in [0.2, 0.3, 0.4, 2.5] { tally.add(cpuSampleMs: ms) }
        XCTAssertEqual(tally.outcome.metrics["cpu_sample_ms_p50"], .number(0.3, digits: 2))
        XCTAssertEqual(tally.outcome.metrics["cpu_sample_ms_max"], .number(2.5, digits: 2))
    }

    /// 25 Nov 19:15: main-thread CPU per test over 100 ms against 40 ms on 24 Nov; the line now says what the main
    /// thread spent between tests, and which tests ran past the 15 s budget.
    func testTheLineCarriesTheMainThreadsCPUAndTheTestsOverBudget() {
        var tally = HarnessTally()
        for ms in [2.0, 3.0, 4.0, 250.0] { tally.add(boundaryMs: 20, flushMs: 10, mainWaitMs: 1, mainCpuMs: ms) }
        tally.add(overBudget: "gpu.x1.pass_pixel_barrier")
        tally.add(overBudget: "gpu.gm20b.zcull")
        XCTAssertEqual(tally.outcome.metrics["main_cpu_ms_p50"], .number(3, digits: 1))
        XCTAssertEqual(tally.outcome.metrics["main_cpu_ms_max"], .number(250, digits: 1))
        XCTAssertEqual(tally.outcome.metrics["over_budget"], .integer(2))
        XCTAssertEqual(tally.outcome.metrics["over_budget_ids"], .text("gpu.x1.pass_pixel_barrier,gpu.gm20b.zcull"))
        XCTAssertEqual(tally.outcome.verdict, .pass, "a long test is named, not failed, by the harness")
        XCTAssertEqual(HarnessTally().outcome.metrics["over_budget_ids"], .text("-"))
    }

    func testTheBudgetHoldsUnattendedTestsToFifteenSeconds() {
        XCTAssertTrue(TestBudget.over(274_230.6, interactive: false))
        XCTAssertFalse(TestBudget.over(15_000, interactive: false))
        XCTAssertFalse(TestBudget.over(60_000, interactive: true), "a person's pace is not the test's")
    }

    func testALongRunSaysSoInItsLine() {
        var tally = HarnessTally()
        XCTAssertNil(tally.outcome.metrics["long_runs"])
        tally.longRuns = true
        XCTAssertEqual(tally.outcome.metrics["long_runs"], .flag(true))
    }
}
