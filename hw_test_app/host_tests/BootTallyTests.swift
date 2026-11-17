import Foundation
import HWTestCore
import XCTest

final class BootTallyTests: XCTestCase {
    func testACountOutlivesItsProcessForTheRestOfTheBoot() throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("tally-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: url) }
        XCTAssertEqual(BootTally(url: url, boot: "A").count, 0)
        try BootTally(url: url, boot: "A").add()
        XCTAssertEqual(BootTally(url: url, boot: "A").count, 1, "Run All resumed in a new process of the same boot")
        try BootTally(url: url, boot: "A").add()
        XCTAssertEqual(BootTally(url: url, boot: "A").count, 2)
        XCTAssertEqual(BootTally(url: url, boot: "B").count, 0, "a new boot starts at 0")
        try BootTally(url: url, boot: "B").add()
        XCTAssertEqual(BootTally(url: url, boot: "B").count, 1)
        XCTAssertEqual(BootTally(url: url, boot: "A").count, 0)
    }
}
