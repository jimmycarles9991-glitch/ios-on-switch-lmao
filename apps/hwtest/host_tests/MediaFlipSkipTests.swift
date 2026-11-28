import HWTestCore
import XCTest

final class MediaFlipSkipTests: XCTestCase {
    /// 25 Nov 19:27:54: every counter 0, skipped as "every video set's producers were done at its swap"
    func testNoSetOnWindowsBAndCFailsInsteadOfSkipping() {
        guard case .fail(let why) = MediaFlipSkip.judge(armed: 0, refused: 0, doneAtSwap: 0, fenced: 0, declined: 0)
        else { return XCTFail("all zero skipped") }
        XCTAssertTrue(why.contains("every counter stayed 0"), why)
    }

    func testSetsDoneAtTheirSwapSkipNamingTheCount() {
        XCTAssertEqual(
            MediaFlipSkip.judge(armed: 0, refused: 0, doneAtSwap: 58, fenced: 0, declined: 0),
            .skip("every video set's producers were done at its swap (d_done_at_swap=58)")
        )
    }

    func testDeclinedSetsSkipNamingTheCount() {
        XCTAssertEqual(
            MediaFlipSkip.judge(armed: 0, refused: 0, doneAtSwap: 3, fenced: 4, declined: 4),
            .skip("the video sets that waited for their producers were declined (d_declined=4)")
        )
    }

    func testFencedSetsNeitherArmedNorDeclinedFail() {
        guard case .fail = MediaFlipSkip.judge(armed: 0, refused: 0, doneAtSwap: 0, fenced: 5, declined: 2) else {
            return XCTFail("not failed")
        }
    }

    func testAnArmedSetOrARefusalIsJudged() {
        XCTAssertEqual(MediaFlipSkip.judge(armed: 40, refused: 0, doneAtSwap: 0, fenced: 40, declined: 0), .judge)
        XCTAssertEqual(MediaFlipSkip.judge(armed: 0, refused: 1, doneAtSwap: 0, fenced: 0, declined: 0), .judge)
    }
}
