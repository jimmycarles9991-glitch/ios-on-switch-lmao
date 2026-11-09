import Foundation
import HWTestCore
import XCTest

final class SignatureTests: XCTestCase {
    /// 16x8 BGRA8: the left 8x8 block grey 128, the right one blue 255 with alpha 64.
    func bgra() -> [UInt8] {
        (0..<(16 * 8)).flatMap { i -> [UInt8] in i % 16 < 8 ? [128, 128, 128, 255] : [255, 0, 0, 64] }
    }

    func testBlockMeansAreDecodedInRGBAOrder() {
        let signature = Signature(bytes: bgra(), metalFormat: 80, width: 16, height: 8)
        XCTAssertEqual(signature.means.count, 8)
        XCTAssertEqual(signature.means[0], 128 / 255, accuracy: 1e-6)
        XCTAssertEqual(Array(signature.means[4...]), [0, 0, 1, Float(64) / 255])
    }

    func testAPartialBlockIsTheMeanOfWhatItHolds() {
        let bytes = (0..<(9 * 1)).flatMap { i -> [UInt8] in [UInt8(i == 8 ? 255 : 0)] }
        let signature = Signature(bytes: bytes, metalFormat: 10, width: 9, height: 1)
        XCTAssertEqual(signature.means, [0, 1])
    }

    func testANonFiniteTexelMakesItsBlockNaN() {
        let values: [Float] = [1, .infinity, 2, 3]
        let bytes = values.flatMap { value in withUnsafeBytes(of: value.bitPattern) { Array($0) } }
        let signature = Signature(bytes: bytes, metalFormat: 55, width: 4, height: 1)
        XCTAssertTrue(signature.means[0].isNaN)
        XCTAssertTrue(signature.matches(signature, absolute: 0, relative: 0))
        let finite = Signature(hash: 1, means: [2])
        XCTAssertFalse(Signature(hash: 2, means: [.nan]).matches(finite, absolute: 10, relative: 1))
    }

    func testCloseMeansMatchAndOthersDoNot() {
        let ours = Signature(hash: 1, means: [0.500, 0.250])
        XCTAssertTrue(ours.matches(Signature(hash: 2, means: [0.508, 0.252]), absolute: 0.0118, relative: 0.01))
        XCTAssertFalse(ours.matches(Signature(hash: 2, means: [0.540, 0.250]), absolute: 0.0118, relative: 0.01))
        XCTAssertTrue(ours.matches(Signature(hash: 1, means: []), absolute: 0, relative: 0), "the same bytes")
        XCTAssertFalse(Signature(hash: 3, means: []).matches(Signature(hash: 4, means: []), absolute: 1, relative: 1))
    }

    func testTextRoundTripsAtHalfPrecision() throws {
        let signature = Signature(bytes: bgra(), metalFormat: 80, width: 16, height: 8)
        let parsed = try XCTUnwrap(Signature(text: signature.text))
        XCTAssertEqual(parsed.hash, signature.hash)
        for (a, b) in zip(parsed.means, signature.means) { XCTAssertEqual(a, b, accuracy: 0.001) }
        XCTAssertEqual(Signature(text: "abc")?.means, [])
        XCTAssertNil(Signature(text: "abc:123"))
        XCTAssertNil(Signature(text: "xyz"))
    }

    func testTheTwoPlaneXRFormatIsJudgedByPlaneZeroAlone() {
        let word: [UInt8] = [0x80, 0x01, 0x06, 0x18]
        let rows = { (pad: UInt8) in (0..<2).flatMap { _ in word + word + [UInt8](repeating: pad, count: 8) } }
        let clean = Signature(bytes: rows(0), metalFormat: Signature.twoPlaneXR, width: 2, height: 2)
        let stale = Signature(bytes: rows(0xEE), metalFormat: Signature.twoPlaneXR, width: 2, height: 2)
        XCTAssertEqual(clean, stale, "the pad after plane 0 is not part of the picture")
        XCTAssertEqual(clean.means.count, 3)
        XCTAssertTrue(Signature.padUntouched(rows(0xEE), metalFormat: 550, width: 2, height: 2, sentinel: 0xEE))
        XCTAssertFalse(Signature.padUntouched(rows(0), metalFormat: 550, width: 2, height: 2, sentinel: 0xEE))
        XCTAssertTrue(Signature.padUntouched([1, 2, 3, 4], metalFormat: 80, width: 1, height: 1, sentinel: 0xEE))
    }

    func testAnUnknownFormatComparesByHashAlone() {
        XCTAssertEqual(Signature(bytes: [1, 2, 3, 4], metalFormat: 92, width: 1, height: 1).means, [])
    }

    func testGoldensParseByStateKeyAndAttachment() throws {
        let signature = Signature(bytes: bgra(), metalFormat: 80, width: 16, height: 8)
        let text =
            "# QuartzCore 24B119 on Apple M4 Max\n"
            + CAGoldens.line(key: "c00c", label: "PBGRAXm_A2Xhf", [(index: 1, signature: signature)]) + "\nbad line\n"
        let goldens = CAGoldens(text)
        XCTAssertEqual(goldens.header, "QuartzCore 24B119 on Apple M4 Max")
        XCTAssertEqual(goldens.states.count, 1)
        XCTAssertEqual(try XCTUnwrap(goldens.states["c00c"]?[1]).hash, signature.hash)
    }
}
