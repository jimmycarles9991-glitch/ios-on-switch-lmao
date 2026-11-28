import Foundation
import HWTestCore
import XCTest

/// The Mac golden files as the device reads them (CAGoldens): none has a line that names no signature, which the
/// device would report as its picture's or state's golden malformed.
final class GoldenFilesTests: XCTestCase {
    func testEveryGoldenLineGivesItsKeyASignature() throws {
        let environment = ProcessInfo.processInfo.environment
        let root = environment["TEST_SRCDIR"].map { $0 + "/" } ?? ""
        let paths = try XCTUnwrap(environment["HWTEST_GOLDEN_FILES"]).split(separator: " ")
        XCTAssertEqual(paths.count, 4)
        for path in paths {
            let text = try String(contentsOf: URL(fileURLWithPath: root + path), encoding: .utf8)
            XCTAssertEqual(CAGoldens(text).malformed, [:], String(path))
        }
    }

    /// gpu.ca.sweep fails before drawing on a Core Animation reference of another GPU
    func testEveryCoreAnimationGoldenIsTheReferenceGPUs() throws {
        let environment = ProcessInfo.processInfo.environment
        let root = environment["TEST_SRCDIR"].map { $0 + "/" } ?? ""
        let paths = try XCTUnwrap(environment["HWTEST_GOLDEN_FILES"]).split(separator: " ").filter {
            $0.split(separator: "/").last?.hasPrefix("ca_") == true
        }
        XCTAssertEqual(paths.count, 2)
        for path in paths {
            let text = try String(contentsOf: URL(fileURLWithPath: root + path), encoding: .utf8)
            XCTAssertEqual(CAGoldens(text).gpu, CAGoldens.referenceGPU, String(path))
        }
    }

    func testTheHeadersGPUIsTheTextBetweenOnAndTheComma() {
        XCTAssertEqual(CAGoldens("# QuartzCore 24B119 on Apple M4, 72x40, 8x8 block means\n").gpu, "Apple M4")
        XCTAssertNil(CAGoldens("# QuartzCore 24B119\n").gpu)
    }
}
