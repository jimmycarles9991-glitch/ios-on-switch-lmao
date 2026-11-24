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
}
