import Foundation
import HWTestCore
import XCTest

final class LandscapeRunTests: XCTestCase {
    /// A reply as drivers/NativeMobileDisplay/SwitchLandscapeRunDisplay.h lays it out.
    func reply(crc: [UInt32], mode: [UInt8], vicJobs: [UInt32] = [0, 0, 0], latches: UInt32 = 0) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: LandscapeSample.size)
        func put(_ word: UInt32, _ at: Int) {
            for i in 0..<4 { bytes[at + i] = UInt8(truncatingIfNeeded: word >> (8 * i)) }
        }
        put(LandscapeSample.magic, 0)
        put(LandscapeSample.done, 4)
        put(UInt32(crc.count), 8)
        put(LandscapeSample.columnScanYuv, 16)
        put(1, 20)
        put(latches, 24)
        put(3, 36)
        for (i, jobs) in vicJobs.enumerated() { put(jobs, 44 + 4 * i) }
        put(7, 68)
        put(2, 80)
        put(1, 84)
        for (i, value) in crc.enumerated() { put(value, 96 + 4 * i) }
        for (i, value) in mode.enumerated() { bytes[96 + 4 * LandscapeSample.maxFrames + i] = value }
        put(4, 96 + 5 * LandscapeSample.maxFrames)
        put(3, 100 + 5 * LandscapeSample.maxFrames)
        return bytes
    }

    func sample(
        _ crc: [UInt32], _ mode: [UInt8], switches: Int = 0, vicJobs: [Int] = [0, 0, 0],
        vicTurns: [Int] = [0, 0, 0], latches: Int = 0
    )
        -> LandscapeSample
    {
        var s = LandscapeSample()
        s.state = LandscapeSample.done
        s.crc = crc
        s.mode = mode
        s.switches = switches
        s.vicJobs = vicJobs
        s.vicTurns = vicTurns
        s.overlayLatches = latches
        return s
    }

    func testTheRequestIsMagicOpValueAndZero() {
        XCTAssertEqual(
            LandscapeSample.request(op: 1, value: 2), [0x56, 0x53, 0x4c, 0x44, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0])
    }

    func testTheReplyCarriesEveryFrameAndCounter() throws {
        let parsed = try XCTUnwrap(
            LandscapeSample(reply(crc: [1, 2, 3], mode: [0, 1, 3], vicJobs: [5, 6, 8], latches: 9)))
        XCTAssertEqual(parsed.crc, [1, 2, 3])
        XCTAssertEqual(parsed.mode, [0, 1, 3])
        XCTAssertEqual(parsed.vicJobs, [5, 6, 8])
        XCTAssertEqual(parsed.overlayLatches, 9)
        XCTAssertEqual(parsed.underflows, [0, 3, 0])
        XCTAssertEqual(parsed.vicEventTurns, [7, 0, 0])
        XCTAssertEqual(parsed.displayNs, 0x1_0000_0002)
        XCTAssertEqual(parsed.landscapeFrames, 2)
        XCTAssertEqual(parsed.failures, 4)
        XCTAssertNil(LandscapeSample([UInt8](repeating: 0, count: LandscapeSample.size)))
    }

    func samples(
        enter: [UInt32]? = nil, vicJobs: [Int] = [0, 0, 0], vicTurns: [Int] = [90, 0, 0]
    ) -> LandscapeJudge.Samples {
        var s = LandscapeJudge.Samples()
        s.vicHeld = sample(Array(repeating: 0xaaaa, count: 30), Array(repeating: 0, count: 30))
        let entering = enter ?? Array(repeating: 0xaaaa, count: 20) + Array(repeating: 0xbbbb, count: 70)
        s.enter = sample(entering, Array(repeating: 0, count: 22) + Array(repeating: 1, count: 68), switches: 1)
        s.playing = sample(
            Array(0..<180).map { UInt32($0 + 1) }, Array(repeating: 1, count: 180), vicJobs: vicJobs,
            vicTurns: vicTurns, latches: 90)
        s.landscapeHeld = sample(Array(repeating: 0xbbbb, count: 30), Array(repeating: 1, count: 30))
        s.leave = sample(
            Array(repeating: 0xbbbb, count: 20) + Array(repeating: 0xaaaa, count: 70),
            Array(repeating: 1, count: 22) + Array(repeating: 0, count: 68), switches: 1)
        return s
    }

    func testAClearRunPasses() {
        let (checks, metrics) = LandscapeJudge.judge(samples(), dropped: 0)
        XCTAssertEqual(checks.filter { !$0.1 }.map(\.0), [])
        XCTAssertEqual(metrics["crc_same"], .flag(false))
        XCTAssertEqual(metrics["vic_jobs_per_frame"], .number(0, digits: 3))
    }

    func testAFrameShowingNeitherRouteFailsTheChange() {
        var enter = Array(repeating: UInt32(0xaaaa), count: 20) + Array(repeating: UInt32(0xbbbb), count: 70)
        enter[21] = 0x1234
        let (checks, metrics) = LandscapeJudge.judge(samples(enter: enter), dropped: 0)
        XCTAssertEqual(checks.filter { !$0.1 }.map(\.0), ["no stray frame going landscape (stray_enter 0)"])
        XCTAssertEqual(metrics["stray_enter"], .integer(1))
    }

    func testAVicJobFromTheDisplayServerWhilePlayingFails() {
        let (checks, metrics) = LandscapeJudge.judge(samples(vicJobs: [90, 0, 0]), dropped: 0)
        XCTAssertEqual(
            checks.filter { !$0.1 }.map(\.0),
            ["no VIC job from the display server for the landscape video (vic_jobs_per_frame 0)"])
        XCTAssertEqual(metrics["vic_jobs_per_frame"], .number(1, digits: 3))
    }

    func testOtherProcessesVicJobsAreReportedNotJudged() {
        let (checks, metrics) = LandscapeJudge.judge(samples(vicJobs: [0, 12, 40]), dropped: 0)
        XCTAssertEqual(checks.filter { !$0.1 }.map(\.0), [])
        XCTAssertEqual(metrics["vic_jobs_test"], .integer(12))
        XCTAssertEqual(metrics["vic_jobs_other"], .integer(40))
    }

    func testTurnsFromAnotherProcessDoNotPassTheRoute() {
        let (checks, _) = LandscapeJudge.judge(samples(vicTurns: [0, 0, 90]), dropped: 0)
        XCTAssertEqual(
            checks.filter { !$0.1 }.map(\.0),
            ["the display server's requests were recorded as quarter turns (vic_turns)"])
    }

    func testZeroReadingsAreNotFrames() {
        var enter = Array(repeating: UInt32(0), count: 6) + Array(repeating: UInt32(0xaaaa), count: 14)
        enter += Array(repeating: UInt32(0xbbbb), count: 70)
        let (checks, metrics) = LandscapeJudge.judge(samples(enter: enter), dropped: 0)
        XCTAssertEqual(checks.filter { !$0.1 }.map(\.0), [])
        XCTAssertEqual(metrics["stray_enter"], .integer(0))
    }

    /// Boot 21's run: the change into landscape came after its sample (all 90 readings the copy route's), and the change
    /// back came 3 frames into its sample, after which the copy route never showed crc_vic. The landscape CRC is the held
    /// landscape frame's, so only the frames after the leave are stray, and the change that missed its sample says so.
    func testAChangeAfterItsSampleIsTimingAndTheLeaveIsJudgedOnItsOwnFrames() {
        var s = samples()
        s.vicHeld = sample(Array(repeating: 0x8a0c_21db, count: 30), Array(repeating: 0, count: 30))
        s.enter = sample(Array(repeating: 0x8a0c_21db, count: 90), Array(repeating: 0, count: 90))
        s.landscapeHeld = sample(Array(repeating: 0xcccc, count: 30), Array(repeating: 1, count: 30))
        s.leave = sample(
            Array(repeating: 0xcccc, count: 3) + Array(repeating: 0xdddd, count: 87),
            Array(repeating: 1, count: 3) + Array(repeating: 0, count: 87), switches: 1)
        let (checks, metrics) = LandscapeJudge.judge(s, dropped: 0)
        XCTAssertEqual(metrics["crc_landscape"], .text("0000cccc"))
        XCTAssertEqual(metrics["crc_back"], .text("0000dddd"))
        XCTAssertEqual(metrics["enter_switched"], .flag(false))
        XCTAssertEqual(metrics["leave_switched"], .flag(true))
        XCTAssertEqual(metrics["stray_enter"], .integer(0))
        XCTAssertEqual(metrics["stray_leave"], .integer(87))
        XCTAssertEqual(metrics["leave_vic_jobs"], .integer(0))
        XCTAssertEqual(metrics["leave_vic_turns"], .integer(0))
        XCTAssertEqual(
            checks.filter { !$0.1 }.map(\.0),
            [
                "the display went landscape on that frame (switches, crc_landscape settled)",
                "back to the VIC route's very frame (crc_vic again, stray_leave 0)",
            ])
    }

    func testDroppedFramesFail() {
        let (checks, _) = LandscapeJudge.judge(samples(), dropped: 2)
        XCTAssertEqual(checks.filter { !$0.1 }.map(\.0), ["no frame dropped"])
    }
}
