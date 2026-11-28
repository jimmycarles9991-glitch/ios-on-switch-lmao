import HWTestCore
import XCTest

/// gpu.x1.l2_flush_writeback's comparison and verdict on made-up rounds: what each outcome of the device run reads as.
final class L2WritebackTests: XCTestCase {
    typealias Arm = L2Writeback.Arm

    func testPatternsAreDistinctNormalFloats() {
        var seen = Set<UInt32>()
        for arm in Arm.allCases {
            for round in 0 ..< L2Writeback.rounds {
                for word in L2Writeback.pattern(arm, round: round) + L2Writeback.sentinel(arm, round: round) {
                    let exponent = word >> 23 & 0xff
                    XCTAssertTrue(exponent != 0 && exponent != 0xff, String(word, radix: 16))
                    XCTAssertTrue(seen.insert(word).inserted, String(word, radix: 16))
                }
            }
        }
    }

    func testCompareCountsWrongWordsAndTheFill() {
        let pattern = L2Writeback.pattern(.memOpD, round: 2)
        let fill = L2Writeback.sentinel(.memOpD, round: 2)
        var words = Array(repeating: pattern, count: 8).flatMap { $0 }
        words[5] = fill[1]
        words[6] = fill[2]
        words[30] = 0x1234
        let c = L2Writeback.compare(words, pattern: pattern, sentinel: fill)
        XCTAssertEqual(c.words, 32)
        XCTAssertEqual(c.wrong, 3)
        XCTAssertEqual(c.sentinel, 2)
        XCTAssertEqual(c.firstWrong, 5)
        XCTAssertEqual(c.firstSeen, fill[1])
        let exact = Array(repeating: pattern, count: 8).flatMap { $0 }
        XCTAssertEqual(L2Writeback.compare(exact, pattern: pattern, sentinel: fill).wrong, 0)
    }

    static func tally(rounds: Int = L2Writeback.rounds, stale: Int = 0, failure: String? = nil) -> L2Writeback.Tally {
        var t = L2Writeback.Tally()
        for round in 0 ..< rounds {
            var c = L2Writeback.Comparison()
            c.words = 16
            if round < stale {
                c.wrong = 16
                c.sentinel = 16
                c.firstWrong = 4
                c.firstSeen = 0x4100_0001
            }
            t.add(c)
        }
        t.failure = failure
        return t
    }

    static func tallies(_ changed: [Arm: L2Writeback.Tally]) -> [Arm: L2Writeback.Tally] {
        var all: [Arm: L2Writeback.Tally] = [.noFlush: tally(stale: 3), .bracket: tally(stale: 2)]
        for arm in Arm.allCases where arm.flushes { all[arm] = tally() }
        return all.merging(changed) { $1 }
    }

    func testExactFlushesWithStaleControlsPass() {
        let finding = L2Writeback.judge(Self.tallies([:]))
        XCTAssertEqual(finding.verdict, .pass, finding.note)
        XCTAssertEqual(finding.metrics["no_flush_stale"], .text("3/8"))
        XCTAssertEqual(finding.metrics["no_flush_first_off"], .text("0x10"))
        XCTAssertEqual(finding.metrics["mem_op_d_stale"], .text("0/8"))
    }

    func testAStaleFlushArmFailsWithItsWordsAndOffset() {
        let finding = L2Writeback.judge(Self.tallies([.memOpD: Self.tally(stale: 1), .jobEnd: Self.tally(stale: 2)]))
        XCTAssertEqual(finding.verdict, .fail)
        let notes = finding.note.components(separatedBy: "; ")
        XCTAssertEqual(notes, [
            "mem_op_d: 16 words off in 1/8 rounds, first at byte 0x10",
            "job_end: 32 words off in 2/8 rounds, first at byte 0x10",
        ])
        XCTAssertEqual(finding.metrics["mem_op_d_fill_words"], .integer(16))
    }

    func testAStaleHandArmBesideAnExactJobEndIsTheProbesPlacement() {
        let killed = Self.tally(rounds: 0, failure: "round 0 killed e31")
        let finding = L2Writeback.judge(Self.tallies([.memOpD: Self.tally(stale: 1), .memOpB: killed]))
        XCTAssertEqual(finding.verdict, .inconclusive)
        let lead = "the hand-emitted methods ran before the pattern reached the L2"
        XCTAssertTrue(finding.note.hasPrefix(lead), finding.note)
        XCTAssertTrue(finding.note.contains("mem_op_b unjudged"), finding.note)
    }

    func testAKilledMemOpBFails() {
        let killed = Self.tally(rounds: 0, failure: "round 0 killed e31: the kernel killed the channel")
        let finding = L2Writeback.judge(Self.tallies([.memOpB: killed]))
        XCTAssertEqual(finding.verdict, .fail)
        XCTAssertTrue(finding.note.hasPrefix("mem_op_b: round 0 killed e31"), finding.note)
    }

    func testAnExactControlLeavesTheFlushUnproven() {
        let finding = L2Writeback.judge(Self.tallies([.noFlush: Self.tally()]))
        XCTAssertEqual(finding.verdict, .inconclusive)
        XCTAssertEqual(finding.note, "unproven: the control read exact")
    }

    func testAnExactBracketLeavesTheMemOpUnproven() {
        let finding = L2Writeback.judge(Self.tallies([.bracket: Self.tally()]))
        XCTAssertEqual(finding.verdict, .inconclusive)
        XCTAssertEqual(finding.note, "unproven: the WFI and FB_FLUSH bracket alone read exact")
    }

    func testAShortFlushArmFails() {
        let finding = L2Writeback.judge(Self.tallies([.jobEnd: Self.tally(rounds: 5)]))
        XCTAssertEqual(finding.verdict, .fail)
        XCTAssertEqual(finding.note, "job_end: ran 5 of 8 rounds")
    }
}
