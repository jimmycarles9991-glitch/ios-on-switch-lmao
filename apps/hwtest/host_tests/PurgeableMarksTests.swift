import HWTestCore
import XCTest

final class PurgeableMarksTests: XCTestCase {
    func testTheKernelLineReadsBack() {
        XCTAssertEqual(
            PurgeableMarks.parse("ok=3 unsupported=2 compressed=12 other=0"),
            PurgeableMarks(ok: 3, unsupported: 2, compressed: 12, other: 0)
        )
        XCTAssertNil(PurgeableMarks.parse("ok=3 unsupported=2"))
    }

    /// 25 Nov 19:18: SpringBoard's marks refused 45 (an old card's volume) and SplashBoard's compressed snapshots
    /// refused; each cause fails on its own
    func testEachCauseFailsItsOwnCheck() {
        let marks = PurgeableMarks(ok: 0, unsupported: 2, compressed: 12, other: 0)
        XCTAssertEqual(marks.checks.map(\.1), [false, false, true, false])
        XCTAssertEqual(marks.asked, 14)
        XCTAssertEqual(
            PurgeableMarks(ok: 40, unsupported: 0, compressed: 0, other: 0).checks.map(\.1),
            [true, true, true, true]
        )
    }

    func testARefusalNamesTheProcessItFirstCameFrom() throws {
        let marks = try XCTUnwrap(
            PurgeableMarks.parse(
                "ok=3 unsupported=0 compressed=0 other=2 unsupported_by=- compressed_by=- other_by=OTACrashCopier"
            )
        )
        XCTAssertEqual(marks.by["other"], "OTACrashCopier")
        XCTAssertEqual(marks.checks[2].0, "no mark refused otherwise (2, first from OTACrashCopier)")
        XCTAssertEqual(marks.checks[1].0, "no mark refused on a compressed snapshot (0)")
    }
}
