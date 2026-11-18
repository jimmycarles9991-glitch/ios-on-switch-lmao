import Foundation
import HWTestCore
import XCTest

final class EmbeddedEntitlementsTests: XCTestCase {
    func bigEndian(_ value: UInt32) -> [UInt8] { withUnsafeBytes(of: value.bigEndian) { Array($0) } }
    func littleEndian(_ value: UInt32) -> [UInt8] { withUnsafeBytes(of: value.littleEndian) { Array($0) } }

    /// A thin arm64 Mach-O with one load command, LC_CODE_SIGNATURE, after `code`: a SuperBlob holding a dummy
    /// CodeDirectory (slot 0) and, when given, the entitlements as XML (slot 5).
    func image(entitlements: [String: Any]?) throws -> Data {
        let directory = bigEndian(0xFADE_0C02) + bigEndian(12) + bigEndian(0)
        var blobs: [(UInt32, [UInt8])] = [(0, directory)]
        if let entitlements {
            let xml = try PropertyListSerialization.data(fromPropertyList: entitlements, format: .xml, options: 0)
            blobs.append((5, bigEndian(0xFADE_7171) + bigEndian(UInt32(8 + xml.count)) + Array(xml)))
        }
        var index: [UInt8] = []
        var payload: [UInt8] = []
        var offset = 12 + 8 * blobs.count
        for (slot, blob) in blobs {
            index += bigEndian(slot) + bigEndian(UInt32(offset))
            payload += blob
            offset += blob.count
        }
        let superBlob =
            bigEndian(0xFADE_0CC0) + bigEndian(UInt32(offset)) + bigEndian(UInt32(blobs.count)) + index
            + payload
        let code: [UInt8] = Array(repeating: 0xAA, count: 16)
        let header = [0xFEED_FACF, 0x0100_000C, 0, 2, 1, 16, 0, 0].flatMap { littleEndian($0) }
        let command = [0x1D, 16, UInt32(48 + code.count), UInt32(superBlob.count)].flatMap { littleEndian($0) }
        return Data(header + command + code + superBlob)
    }

    func testTheSignedEntitlementsAreRead() throws {
        let signed: [String: Any] = ["platform-application": true, "application-identifier": "SWITCH.x"]
        let read = try XCTUnwrap(EmbeddedEntitlements.of(image(entitlements: signed)))
        XCTAssertEqual(NSDictionary(dictionary: read), NSDictionary(dictionary: signed))
    }

    func testASignatureWithoutEntitlementsHasNone() throws {
        XCTAssertEqual(try XCTUnwrap(EmbeddedEntitlements.of(image(entitlements: nil))).count, 0)
    }

    func testAnythingElseIsUnreadable() throws {
        XCTAssertNil(EmbeddedEntitlements.of(Data("#!/bin/sh\n".utf8)))
        XCTAssertNil(EmbeddedEntitlements.of(try image(entitlements: ["a": true]).prefix(60)))
    }
}
