import Foundation
import HWTestCore
import XCTest

final class KeylineClaimTests: XCTestCase {
    func evidence(
        line: [Int], backdrop: [Int]? = [20, 20, 21], framebuffer: [Int]? = nil, replica: String? = "",
        fill: [Double]? = nil
    ) -> KeylineClaim.Evidence {
        KeylineClaim.Evidence(
            line: line, backdrop: backdrop, framebuffer: framebuffer, replicaWrong: replica, alpha: 0.1, fill: fill)
    }

    func testAMagentaFillIsUIKitsOwnBeforeAnyPixelReasoning() {
        let magenta: [Double] = [1, 0, 1, 1]
        let fresh = evidence(line: [255, 0, 255], framebuffer: [255, 0, 255], fill: magenta)
        XCTAssertEqual(KeylineClaim.verdict(fresh).claim, "fill")
        XCTAssertEqual(
            KeylineClaim.verdict(evidence(line: [255, 0, 255], replica: "c0.g", fill: magenta)).claim, "fill")
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [255, 0, 255], replica: nil, fill: magenta)).claim, "fill")
        let white = evidence(line: [255, 0, 255], framebuffer: [255, 0, 255], fill: [1, 1, 1, 0.1])
        XCTAssertEqual(KeylineClaim.verdict(white).claim, "b")
    }

    func testTheFillIsJudgedAgainstTheDeviceClassesFill() {
        let rows: [[Double]] = [[1, 1, 1, 0.1], [0, 0, 0, 0.1]]
        XCTAssertEqual(KeylineClaim.fillVerdict([1, 1, 1, Double(Float(0.1))], wants: rows), "want")
        XCTAssertEqual(KeylineClaim.fillVerdict([0, 0, 0, 0.1], wants: rows), "want")
        XCTAssertEqual(KeylineClaim.fillVerdict([1, 0, 1, 1], wants: rows), "magenta")
        XCTAssertEqual(KeylineClaim.fillVerdict([0, 0, 0, 0.1], wants: [rows[0]]), "other")
        XCTAssertEqual(KeylineClaim.fillVerdict([1, 1, 1, 1], wants: rows), "other")
        XCTAssertEqual(KeylineClaim.fillVerdict(nil, wants: rows), "other")
        XCTAssertFalse(KeylineClaim.magenta([1, 0, 1, 0.5]))
    }

    func testTheKeylinesWhiteOverTheBackdropIsSourceOverOfTheHalfAlpha() {
        XCTAssertEqual(KeylineClaim.half(0.1), 0.0999755859375)
        XCTAssertEqual(KeylineClaim.expected(over: [20, 20, 21], alpha: 0.1), [43, 43, 44])
        XCTAssertEqual(KeylineClaim.expected(over: [255, 0, 128], alpha: 0.1), [255, 25, 141])
    }

    func testTheDevicesRowIsTheFreshTexelAndBNamesItWhenTheReplicaIsExact() {
        let (claim, why) = KeylineClaim.verdict(evidence(line: [255, 0, 255], framebuffer: [255, 0, 255]))
        XCTAssertEqual(claim, "b")
        XCTAssertTrue(why.contains("fresh"))
        XCTAssertTrue(KeylineClaim.fresh([254, 0, 254]))
        XCTAssertFalse(KeylineClaim.fresh([255, 25, 255]))
    }

    func testAWrongReplicaIsTheDrawsOwn() {
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [255, 0, 255], replica: "c0.g")).claim, "a")
    }

    func testAPurpleRowWithoutAReplicaDecidesNeither() {
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [250, 10, 240], replica: nil)).claim, "a_or_b")
    }

    func testCleanRenderAndFramebufferPutTheLineAfterTheFramebuffer() {
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [43, 43, 44], framebuffer: [43, 43, 44])).claim, "d")
    }

    func testAChannelOffByTheROPsRoundingIsC() {
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [43, 42, 44])).claim, "c")
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [43, 42, 44], framebuffer: [43, 42, 44])).claim, "c")
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [43, 43, 44])).claim, "none")
    }

    func testAPurpleFramebufferUnderACleanSnapshotIsB() {
        XCTAssertEqual(KeylineClaim.verdict(evidence(line: [43, 43, 44], framebuffer: [255, 0, 255])).claim, "b")
    }
}
