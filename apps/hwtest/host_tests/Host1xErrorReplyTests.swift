import Foundation
import HWTestCore
import XCTest

final class Host1xErrorReplyTests: XCTestCase {
    func reply(
        status: Int32 = 0, attached: UInt32 = 1, taken: UInt32 = 1, stopped: UInt32 = 0x2000, recovered: UInt32 = 1,
        intmaskAfter: UInt32 = 0x11
    ) -> [UInt8] {
        let lines: [UInt32] = [0x11, 1 << 31, 0, 0x1fff, intmaskAfter, 0, 0, 0x3fff]
        return ([Host1xErrorReply.magic, UInt32(bitPattern: status), attached, 13, 15, taken, 0, stopped, 0, recovered]
            + lines).flatMap { word in (0 ..< 4).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) } }
    }

    func testDeliveredAndRecoveredIsAPass() throws {
        let parsed = try XCTUnwrap(Host1xErrorReply(reply()))
        XCTAssertNil(parsed.failure)
        XCTAssertEqual(parsed.statStopped, 0x2000)
        XCTAssertEqual(parsed.stopped.hintstatus, 1 << 31)
        XCTAssertEqual(parsed.stopped.hintmaskExt, 0x1fff)
        XCTAssertEqual(parsed.after.hintmaskExt, 0x3fff)
        XCTAssertEqual(Host1xErrorReply.request, [0x50, 0x4F, 0x49, 0x48, 0, 0, 0, 0])
    }

    func testEachMissingStepFailsWithItsOwnReason() throws {
        XCTAssertEqual(try XCTUnwrap(Host1xErrorReply(reply(attached: 0, taken: 0))).failure,
                       "host1x's general interrupt is not attached")
        XCTAssertEqual(try XCTUnwrap(Host1xErrorReply(reply(status: -22, taken: 0))).failure,
                       "the illegal opcode could not be submitted (-22)")
        XCTAssertEqual(try XCTUnwrap(Host1xErrorReply(reply(taken: 0))).failure,
                       "nothing latched: opcode 15 is not illegal on this host1x")
        XCTAssertEqual(try XCTUnwrap(Host1xErrorReply(reply(recovered: 0))).failure,
                       "channel 13 ran no stream after its restart")
        XCTAssertEqual(try XCTUnwrap(Host1xErrorReply(reply(intmaskAfter: 0x10))).failure,
                       "the general interrupt's master enable went off (INTMASK 0x10)")
    }

    func testAReplyOfAnotherSizeOrMagicIsRefused() {
        XCTAssertNil(Host1xErrorReply(Array(reply().prefix(40))))
        var bytes = reply()
        bytes[0] ^= 1
        XCTAssertNil(Host1xErrorReply(bytes))
    }
}
