import Foundation
import HWTestCore
import XCTest

final class DisplayWindowApertureReplyTests: XCTestCase {
    func reply(error: Int32 = 0, words: UInt32 = 0xf, aliased: UInt32, restored: UInt32 = 0xf) -> [UInt8] {
        [DisplayWindowProbeReply.magic, UInt32(bitPattern: error), words, aliased, restored, 0].flatMap { word in
            (0..<4).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) }
        }
    }

    func testEveryWordAliasedAndRestoredIsTheAlias() throws {
        let parsed = try XCTUnwrap(DisplayWindowApertureReply(reply(aliased: 0xf)))
        XCTAssertTrue(parsed.alias)
        XCTAssertEqual(parsed.words, 0xf)
    }

    func testOneWordThatDoesNotAliasOrComeBackIsNot() throws {
        XCTAssertFalse(try XCTUnwrap(DisplayWindowApertureReply(reply(aliased: 0xe))).alias)
        XCTAssertFalse(try XCTUnwrap(DisplayWindowApertureReply(reply(aliased: 0xf, restored: 0x7))).alias)
        XCTAssertFalse(try XCTUnwrap(DisplayWindowApertureReply(reply(error: 16, words: 0xf, aliased: 0))).alias)
    }

    func testAReplyOfAnotherSizeOrMagicIsRefused() {
        XCTAssertNil(DisplayWindowApertureReply(Array(reply(aliased: 0xf).prefix(20))))
        var bytes = reply(aliased: 0xf)
        bytes[0] ^= 1
        XCTAssertNil(DisplayWindowApertureReply(bytes))
    }
}
