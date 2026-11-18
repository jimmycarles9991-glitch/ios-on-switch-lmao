import Foundation
import HWTestCore
import XCTest

final class DisplayWindowProbeReplyTests: XCTestCase {
    /// A reply as drivers/NativeMobileDisplay/DisplayWindowProbeRun.h replyOf lays it out.
    func reply(verdict: UInt32, steps: [(String, UInt32, UInt8, UInt8)], why: String = "") -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: DisplayWindowProbeReply.size)
        func put(_ word: UInt32, _ at: Int) {
            for i in 0..<4 { bytes[at + i] = UInt8(truncatingIfNeeded: word >> (8 * i)) }
        }
        put(DisplayWindowProbeReply.magic, 0)
        put(3, 4)
        put(2, 8)
        put(verdict, 12)
        put(UInt32(steps.count), 16)
        put(1, 20)
        put(42, 24)
        for (i, c) in why.utf8.enumerated() { bytes[32 + i] = c }
        for (i, step) in steps.enumerated() {
            let at = 96 + 28 * i
            put(step.1, at)
            bytes[at + 4] = 1
            bytes[at + 8] = step.2
            bytes[at + 9] = step.3
            for (j, c) in step.0.utf8.enumerated() { bytes[at + 12 + j] = c }
        }
        return bytes
    }

    func testTheRequestIsMagicOpProbeAndZero() {
        XCTAssertEqual(
            DisplayWindowProbeReply.request(op: 1, probe: 3),
            [0x52, 0x50, 0x57, 0x44, 1, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0])
    }

    func testAPassCarriesEveryStepsCrcAndItsComparison() throws {
        let bytes = reply(
            verdict: 0, steps: [("ref_hv", 0xabc, 0, 0), ("crop_hv", 0xabc, 1, 0), ("control", 0x123, 2, 1)])
        let parsed = try XCTUnwrap(DisplayWindowProbeReply(bytes))
        XCTAssertEqual(parsed.probe, 3)
        XCTAssertEqual(parsed.state, DisplayWindowProbeReply.done)
        XCTAssertEqual(parsed.steps.map(\.name), ["ref_hv", "crop_hv", "control"])
        let outcome = parsed.finding.outcome(probe: "V5c", base: Metrics(), proof: "")
        let line = LogLine.format(id: "display.x1.offset_crop", outcome: outcome, milliseconds: 1)
        XCTAssertTrue(line.hasPrefix("HWTEST display.x1.offset_crop PASS probe=V5c claim_verdict=PASS"), line)
        XCTAssertTrue(line.contains("crop_hv_crc=00000abc"), line)
        XCTAssertTrue(line.contains("crop_hv_same=1"), line)
        XCTAssertTrue(line.contains("control_same=0"), line)
        XCTAssertFalse(line.contains("ref_hv_same"), line)
    }

    func testAFailCarriesTheKernelsReason() throws {
        let bytes = reply(
            verdict: 1, steps: [("pitch", 1, 0, 0), ("block2", 2, 1, 0)], why: "a step's CRC is not its reference's")
        let finding = try XCTUnwrap(DisplayWindowProbeReply(bytes)).finding
        XCTAssertEqual(finding.verdict, .fail)
        XCTAssertEqual(finding.note, "a step's CRC is not its reference's")
    }

    func testAShortOrForeignReplyIsRefused() {
        XCTAssertNil(DisplayWindowProbeReply([UInt8](repeating: 0, count: 320)))
        XCTAssertNil(DisplayWindowProbeReply([UInt8](repeating: 0, count: 12)))
    }
}
