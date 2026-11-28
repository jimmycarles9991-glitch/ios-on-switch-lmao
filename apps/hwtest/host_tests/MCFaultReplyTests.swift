import Foundation
import HWTestCore
import XCTest

final class MCFaultReplyTests: XCTestCase {
    func reply(
        status: Int32 = 0, attached: UInt32 = 1, taken: UInt32 = 3, intstatus: UInt32 = 0x400,
        errStatus: UInt32 = 0x6A00_006C, address: UInt32 = 0x0800_0040
    ) -> [UInt8] {
        [MCFaultReply.magic, UInt32(bitPattern: status), 0x0800_0000, attached, taken, intstatus, errStatus, address]
            .flatMap { word in (0 ..< 4).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) } }
    }

    func testTheCopysFaultDeliveredIsAPass() throws {
        let parsed = try XCTUnwrap(MCFaultReply(reply()))
        XCTAssertNil(parsed.failure)
        XCTAssertEqual(parsed.taken, 3)
        XCTAssertEqual(MCFaultReply.request, [0x46, 0x43, 0x4D, 0x56, 0, 0, 0, 0])
        let decerrBytes = reply(intstatus: 0x440, errStatus: 0x6000_006C, address: 0x0800_0000)
        let withDecerr = try XCTUnwrap(MCFaultReply(decerrBytes))
        XCTAssertNil(withDecerr.failure)
    }

    func testNothingDeliveredOrNotAttachedOrRefusedFails() throws {
        XCTAssertNotNil(try XCTUnwrap(MCFaultReply(reply(taken: 0))).failure)
        XCTAssertNotNil(try XCTUnwrap(MCFaultReply(reply(attached: 0, taken: 0))).failure)
        XCTAssertNotNil(try XCTUnwrap(MCFaultReply(reply(status: -95, taken: 0))).failure)
    }

    func testAnotherClientKindOrAddressFails() throws {
        XCTAssertNotNil(try XCTUnwrap(MCFaultReply(reply(errStatus: 0x6A00_006D))).failure)
        XCTAssertNotNil(try XCTUnwrap(MCFaultReply(reply(intstatus: 0x40, errStatus: 0x6000_006C))).failure)
        XCTAssertNotNil(try XCTUnwrap(MCFaultReply(reply(intstatus: 0x440, errStatus: 0x2000_006C))).failure)
        XCTAssertNotNil(try XCTUnwrap(MCFaultReply(reply(intstatus: 0x500))).failure)
        XCTAssertNotNil(try XCTUnwrap(MCFaultReply(reply(address: 0x0801_0000))).failure)
    }

    func testAReplyOfAnotherSizeOrMagicIsRefused() {
        XCTAssertNil(MCFaultReply(Array(reply().prefix(28))))
        var bytes = reply()
        bytes[0] ^= 1
        XCTAssertNil(MCFaultReply(bytes))
    }
}
