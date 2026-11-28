import Foundation
import HWTestCore
import XCTest

final class GlassSidebarVerdictTests: XCTestCase {
    typealias Phase = GlassSidebarVerdict.Phase

    func phases() -> [Phase] {
        [
            Phase(name: "idle1", moving: false),
            Phase(name: "sidebar", moving: true, frames: 240),
            Phase(name: "idle2", moving: false),
            Phase(name: "detail", moving: true, frames: 240),
            Phase(name: "both", moving: true, frames: 120),
        ]
    }

    func testAPassingRun() {
        let outcome = GlassSidebarVerdict.outcome(phases(), scene: Metrics(), pngs: [], stopped: nil)
        XCTAssertEqual(outcome.verdict, .pass)
        XCTAssertEqual(outcome.metrics["frames"], .integer(600))
        XCTAssertEqual(outcome.metrics["sidebar"], .text("240/0/0"))
    }

    func testBlackAndPartialFramesFail() {
        var run = phases()
        run[3] = Phase(name: "detail", moving: true, frames: 240, black: [5012, 5040], partial: 1)
        let outcome = GlassSidebarVerdict.outcome(run, scene: Metrics(), pngs: [], stopped: nil)
        XCTAssertEqual(outcome.verdict, .fail)
        XCTAssertEqual(outcome.metrics["detail"], .text("240/2/1"))
        XCTAssertEqual(outcome.metrics["detail_first"], .integer(5012))
        XCTAssertEqual(
            outcome.detail,
            "detail: 2 of 240 frames show the sidebar black; 1 frames show the sidebar partly black"
        )
    }

    func testTheReasonNamesWhatTheBandShows() {
        let run = [
            Phase(name: "scroll", moving: true, frames: 180),
            Phase(name: "appear", moving: true, frames: 180, black: [812], partial: 3),
        ]
        let outcome = GlassSidebarVerdict.outcome(run, scene: Metrics(), pngs: [], stopped: nil, subject: "glass pane")
        XCTAssertEqual(outcome.verdict, .fail)
        XCTAssertEqual(
            outcome.detail,
            "appear: 1 of 180 frames show the glass pane black; 3 frames show the glass pane partly black"
        )
    }

    func testNoFrameIsASkipAndAStoppedReadBackNeverPasses() {
        let empty = phases().map { Phase(name: $0.name, moving: $0.moving) }
        let none = GlassSidebarVerdict.outcome(empty, scene: Metrics(), pngs: [], stopped: nil)
        XCTAssertEqual(none.verdict, .skip)
        let stopped = GlassSidebarVerdict.outcome(
            Array(phases().prefix(2)), scene: Metrics(), pngs: [], stopped: "sidebar:rearm"
        )
        XCTAssertEqual(stopped.verdict, .skip)
    }

    func testTheWorstLineFitsWhatOsLogKeeps() {
        let worst = phases().map {
            Phase(
                name: $0.name, moving: $0.moving, frames: 999,
                black: Array(repeating: UInt64(UInt32.max), count: 999), partial: 999
            )
        }
        var scene = Metrics()
        scene.set("under", true)
        scene.set("region", "1234,1234,1234,1234")
        scene.set("means", "0.00/0.00")
        let pngs = Array(repeating: "detail-\(UInt32.max)-before.png", count: 9)
        let outcome = GlassSidebarVerdict.outcome(worst, scene: scene, pngs: pngs, stopped: "both:query_0x1")
        let line = LogLine.format(id: "ui.glass_sidebar", outcome: outcome, milliseconds: 123_456.7)
        XCTAssertLessThanOrEqual(line.utf8.count, 1000, line)
    }
}
