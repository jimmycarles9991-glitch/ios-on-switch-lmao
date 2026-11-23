import Foundation
import HWTestCore
import XCTest

/// gpu.path.import_beside_submits' verdict on submissions made up to show each case: ones that wait behind a wiring from a
/// uniformly random point of it, ones that only meet the run's contention, and boot 22's undecidable wiring.
final class ImportBesideVerdictTests: XCTestCase {
    let quiet = (0 ..< 160).map { 0.30 + 0.0003 * Double($0 % 100) }

    func beside(_ count: Int, extra: (Int) -> Double) -> [Double] {
        (0 ..< count).map { 0.30 + 0.0003 * Double($0 % 100) + extra($0) }
    }

    func testSubmissionsBlockedBehindTheWiringFail() {
        let wire = 1.3
        let blocked = beside(120) { wire * Double($0) / 120 }
        let failures = ImportBesideVerdict.failures(wireMs: wire, quiet: quiet, beside: blocked, minimumBeside: 40)
        XCTAssertEqual(failures.count, 1, "\(failures)")
        XCTAssertTrue(failures.first?.hasPrefix("imports added") == true, "\(failures)")
    }

    func testSubmissionsThatOnlyMeetContentionPass() {
        let free = beside(120) { $0 % 30 == 0 ? 0.26 : 0.05 }
        XCTAssertEqual(ImportBesideVerdict.failures(wireMs: 1.3, quiet: quiet, beside: free, minimumBeside: 40), [])
    }

    func testBoot22sWiringCannotTell() {
        let failures = ImportBesideVerdict.failures(
            wireMs: 0.28, quiet: quiet, beside: beside(66) { $0 == 0 ? 0.26 : 0 }, minimumBeside: 40
        )
        XCTAssertTrue(failures.contains { $0.contains("too short to tell") }, "\(failures)")
    }

    func testAQuietRunThatSpreadsNeedsALongerWiring() {
        let spread = (0 ..< 160).map { 0.3 + Double($0) / 160 }
        let failures = ImportBesideVerdict.failures(wireMs: 1.3, quiet: spread, beside: spread, minimumBeside: 40)
        XCTAssertTrue(failures.contains { $0.contains("too short to tell") }, "\(failures)")
    }
}
