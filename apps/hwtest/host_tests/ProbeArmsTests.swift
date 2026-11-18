import Foundation
import HWTestCore
import XCTest

final class ProbeArmsTests: XCTestCase {
    func testTrialsForARaceOfOneInFiveHundred() {
        XCTAssertEqual(ProbeConfidence.trials(rate: 1.0 / 500, confidence: 0.95), 1497)
        XCTAssertEqual(ProbeConfidence.trials(rate: 1.0 / 500, confidence: 0.99), 2301)
        XCTAssertEqual(ProbeConfidence.confidence(trials: 500, rate: 1.0 / 500), 0.632, accuracy: 0.001)
        XCTAssertEqual(ProbeConfidence.bound95(trials: 1500), 3.0 / 1500, accuracy: 0.0001)
        XCTAssertEqual(ProbeConfidence.bound95(trials: 0), 1)
    }

    func testATallyCountsTrialsAndNamesTheFirstTexelThatDiffered() {
        var tally = ArmTally()
        let want: [UInt8] = Array(0..<32)
        tally.compare(want, want, trial: 0, width: 4, bytesPerTexel: 4)
        var got = want
        got[21] = 0xff
        got[30] = 0
        tally.compare(got, want, trial: 1, width: 4, bytesPerTexel: 4)
        tally.compare(got, want, trial: 2, width: 4, bytesPerTexel: 4)
        XCTAssertEqual(tally.trials, 3)
        XCTAssertEqual(tally.mismatchedTrials, 2)
        XCTAssertEqual(tally.mismatchedBytes, 4)
        XCTAssertEqual(tally.first, "1:1,1:14ff1617>14151617")
        XCTAssertFalse(tally.clean)
    }

    func testACellHoldsOnlyWhereTheArmWithNoBarrierShowedTheHazard() {
        var clean = ArmTally()
        clean.compare([1, 2], [1, 2], trial: 0, width: 1, bytesPerTexel: 2)
        var dirty = ArmTally()
        dirty.compare([1, 3], [1, 2], trial: 0, width: 1, bytesPerTexel: 2)
        XCTAssertEqual(CellVerdict.of(relaxed: clean, unguarded: dirty), .holds)
        XCTAssertEqual(CellVerdict.of(relaxed: clean, unguarded: clean), .blind)
        XCTAssertEqual(CellVerdict.of(relaxed: clean, unguarded: nil), .blind)
        XCTAssertEqual(CellVerdict.of(relaxed: dirty, unguarded: dirty), .fails)
        XCTAssertEqual(CellVerdict.claim([.holds, .holds]), .pass)
        XCTAssertEqual(CellVerdict.claim([.holds, .blind]), .inconclusive)
        XCTAssertEqual(CellVerdict.claim([.holds, .holds, .blind]), .pass)
        XCTAssertEqual(CellVerdict.claim([.blind, .blind, .holds]), .inconclusive)
        XCTAssertEqual(CellVerdict.claim([.holds, .fails, .blind]), .fail)
        XCTAssertEqual(CellVerdict.claim([]), .inconclusive)
    }

    func testThePlanVisitsEveryCellBeforeAnyCellsSecondRound() {
        let plan = IterationPlan(cells: 3, target: 20, batch: 8)
        XCTAssertEqual(plan.rounds, 3)
        XCTAssertEqual(plan.order.count, 9)
        XCTAssertEqual(plan.order.prefix(3).map(\.cell), [0, 1, 2])
        XCTAssertEqual(plan.order.prefix(3).map(\.round), [0, 0, 0])
        XCTAssertEqual(plan.order.last?.round, 2)
    }

    func testConsecutiveTrialsTakeDifferentSeeds() {
        for cell in 0..<5 {
            for trial in 0..<200 {
                XCTAssertNotEqual(
                    probeSeed(trial: trial, cell: cell, seeds: 16), probeSeed(trial: trial + 1, cell: cell, seeds: 16))
            }
        }
        XCTAssertEqual(probeHash(0, 0, 0), 0)
        XCTAssertNotEqual(probeHash(1, 0, 0), probeHash(0, 1, 0))
    }
}
