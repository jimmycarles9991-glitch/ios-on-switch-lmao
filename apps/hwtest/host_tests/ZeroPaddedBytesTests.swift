import HWTestCore
import XCTest

final class ZeroPaddedBytesTests: XCTestCase {
    func testFillZeroesEntireDirtyBufferWithoutPrefix() {
        var storage = [UInt8](repeating: 0xa5, count: 65536)
        storage.withUnsafeMutableBytes { ZeroPaddedBytes.fill($0.baseAddress!, length: $0.count) }
        XCTAssertTrue(storage.allSatisfy { $0 == 0 })
    }

    func testFillCopiesPrefixAndZeroesDirtyTail() {
        var storage = [UInt8](repeating: 0xa5, count: 65536)
        let prefix = (0 ..< 64).map { UInt8($0 + 1) }
        storage.withUnsafeMutableBytes { dst in
            prefix.withUnsafeBytes { ZeroPaddedBytes.fill(dst.baseAddress!, length: dst.count, prefix: $0) }
        }
        XCTAssertEqual(Array(storage.prefix(64)), prefix)
        XCTAssertTrue(storage.dropFirst(64).allSatisfy { $0 == 0 })
    }

    func testFillClampsOversizedPrefixToLength() {
        var storage = [UInt8](repeating: 0xa5, count: 16)
        let prefix = [UInt8](repeating: 0x3c, count: 64)
        storage.withUnsafeMutableBytes { dst in
            prefix.withUnsafeBytes { ZeroPaddedBytes.fill(dst.baseAddress!, length: 8, prefix: $0) }
        }
        XCTAssertEqual(Array(storage.prefix(8)), [UInt8](repeating: 0x3c, count: 8))
        XCTAssertEqual(Array(storage.dropFirst(8)), [UInt8](repeating: 0xa5, count: 8))
    }
}
