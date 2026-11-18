import Foundation
import HWTestCore
import XCTest

final class PresentTimeVerdictTests: XCTestCase {
    typealias Frame = PresentTimeVerdict.Frame
    static let period = PresentTimeVerdict.period

    /// A drawable every fourth refresh asked for four refreshes after its commit, shown `offset` seconds after that.
    static func run(_ count: Int = 45, offset: (Int) -> Double? = { _ in 0 }) -> [Frame] {
        (0..<count).map { i in
            let committed = 100 + Double(i) * 4 * period
            let target = committed + 4 * period
            guard let late = offset(i) else { return Frame(committed: committed, target: target, handled: committed) }
            return Frame(committed: committed, target: target, presented: target + late, handled: target + late + 0.001)
        }
    }

    static func failed(_ frames: [Frame]) -> [String] {
        PresentTimeVerdict.checks(frames, minimum: 40).filter { !$0.1 }.map(\.0)
    }

    func testDrawablesShownAtTheirRefreshPass() {
        XCTAssertEqual(Self.failed(Self.run { i in i % 2 == 0 ? 0.0005 : -0.0005 }), [])
    }

    /// Boots 19-22: every handler ran at its present with presentedTime 0.
    func testHandledWithoutATimeFails() {
        let frames = Self.run { _ in nil }
        XCTAssertEqual(Self.failed(frames), ["timed==submitted"])
        XCTAssertEqual(PresentTimeVerdict.Summary(frames).handled, 45)
        XCTAssertTrue(PresentTimeVerdict.offsets(frames).hasPrefix("x,x,x"))
    }

    func testARefreshEarlyOrLateFails() {
        let frames = Self.run { i in i == 3 ? -Self.period : i == 7 ? Self.period : 0 }
        let summary = PresentTimeVerdict.Summary(frames)
        XCTAssertEqual(summary.early, 1)
        XCTAssertEqual(summary.late, 1)
        XCTAssertEqual(Self.failed(frames), ["early==0", "late==0"])
    }

    func testNoRunOrNoLeadCannotPass() {
        XCTAssertTrue(Self.failed([]).contains("submitted>=40"))
        XCTAssertTrue(Self.failed([]).contains("every_target_a_period_ahead"))
        let asap = (0..<45).map { i -> Frame in
            let at = 100 + Double(i) * 4 * Self.period
            return Frame(committed: at, target: at, presented: at + 2 * Self.period, handled: at + 2 * Self.period)
        }
        XCTAssertEqual(Self.failed(asap), ["every_target_a_period_ahead", "late==0"])
    }

    func testAHandlerThatNeverRanFails() {
        var frames = Self.run()
        let last = frames.removeLast()
        frames.append(Frame(committed: last.committed, target: last.target, presented: last.presented))
        XCTAssertEqual(Self.failed(frames), ["handled==submitted"])
    }

    func testOffsetsAndControlCounts() {
        let frames = Self.run(3) { i in i == 1 ? nil : Double(i) * Self.period }
        XCTAssertEqual(PresentTimeVerdict.offsets(frames), "0.0,x,33.3")
        XCTAssertEqual(PresentTimeVerdict.shown(frames), "2/3")
        XCTAssertEqual(frames[0].leadMs, 4000 * Self.period, accuracy: 1e-9)
    }
}
