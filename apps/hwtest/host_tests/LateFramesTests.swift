import Foundation
import HWTestCore
import XCTest

final class LateFramesTests: XCTestCase {
    static let period = 1.0 / 60

    /// Frames every period from `start` for `count` periods, without the ones `dropped` names.
    static func frames(from start: Double, count: Int, dropped: (Int) -> Bool) -> [Double] {
        (0 ..< count).filter { !dropped($0) }.map { start + Double($0) * period }
    }

    /// pacing.glass_under on the 22 Nov device run: a frame missed every 250 ms, always at the same phase.
    func testAMissEvery250MsSitsOnOnePhase() {
        let times = Self.frames(from: 1000.004, count: 180) { $0 % 15 == 7 }
        let late = LateFrames(presented: times, period: Self.period)
        XCTAssertEqual(late.late.count, 12)
        XCTAssertEqual(late.onGrid, 12)
        XCTAssertTrue(late.late.allSatisfy { $0.periods == 2 })
        let phases = Set(late.late.map { Int($0.phase.rounded()) })
        XCTAssertEqual(phases.count, 1, "\(phases)")
    }

    func testMissesOffTheGridDoNotGather() {
        let misses: Set<Int> = [5, 23, 44, 70, 96, 131, 149]
        let late = LateFrames(
            presented: Self.frames(from: 2000, count: 180) { misses.contains($0) },
            period: Self.period
        )
        XCTAssertEqual(late.late.count, misses.count)
        XCTAssertLessThan(late.onGrid, 4)
    }

    /// The phase wraps: 249 ms and 1 ms are two milliseconds apart on the grid.
    func testPhasesWrapAroundTheGrid() {
        let late = LateFrames(presented: [9.000, 9.249, 9.249 + Self.period, 10.001], period: Self.period)
        XCTAssertEqual(late.late.count, 2)
        XCTAssertEqual(late.onGrid, 2)
        XCTAssertEqual(late.phases(limit: 1), "249/15,+1")
        XCTAssertEqual(late.phases(), "249/15,1/44")
    }

    func testUnshownFramesAndNoLateFrames() {
        let late = LateFrames(presented: [0, 3.0, 3.0 + Self.period, 0, 3.0 + 2 * Self.period], period: Self.period)
        XCTAssertEqual(late.intervals, 2)
        XCTAssertEqual(late.late, [])
        XCTAssertEqual(late.onGrid, 0)
        XCTAssertEqual(late.phases(), "-")
    }
}
