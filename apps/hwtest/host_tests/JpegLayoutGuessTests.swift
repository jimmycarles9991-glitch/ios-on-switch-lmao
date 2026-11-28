import HWTestCore
import XCTest

/// 25 Nov 19:20:04: NVJPG's 260x260 decode into a block-linear destination matched no candidate, and `explained` said
/// only `-`. The closest arrangement and its count come out now, near miss or not.
final class JpegLayoutGuessTests: XCTestCase {
    static func picture(_ x: Int, _ y: Int, _ c: Int) -> UInt8 { UInt8(truncatingIfNeeded: x * 7 + y * 13 + c * 61) }

    /// The rows a reader gets from a surface `name` was written into, read back through GM20B's decided blocks
    static func read(_ name: String, width: Int, height: Int) -> [UInt8] {
        let rowBytes = (width * 4 + 63) & ~63
        let candidate = JpegLayoutGuess.candidates(width: width, rowBytes: rowBytes).first { $0.name == name }!
        let span = max(rowBytes, (width * 4 + 255) & ~255) * GobLayout.blockRows(height, 5)
        var memory = [UInt8](repeating: 0, count: span)
        for y in 0 ..< height {
            for xB in 0 ..< width * 4 { memory[candidate.offset(xB, y)] = picture(xB / 4, y, xB % 4) }
        }
        let decided = GobLayout.blockHeightLog2(height)
        var got: [UInt8] = []
        for y in 0 ..< height {
            for xB in 0 ..< width * 4 { got.append(memory[GobLayout.offset(rowBytes, decided, xB, y)]) }
        }
        return got
    }

    func guess(_ got: [UInt8], _ side: Int) -> String {
        JpegLayoutGuess.best(
            got, JpegLayoutGuess.Surface(width: side, height: side, allocation: 1 << 24), tolerance: 0,
            picture: Self.picture
        )
    }

    func testEachArrangementIsToldApartAtNoBytesOff() {
        for name in ["rows", "rows_bare", "y2", "tiled16"] {
            XCTAssertEqual(guess(Self.read(name, width: 260, height: 260), 260), "\(name):0")
        }
    }

    func testANearMissNamesItsClosestArrangementAndCount() {
        var got = Self.read("y4", width: 64, height: 64)
        for at in [5, 900, 4000] { got[at] &+= 100 }
        XCTAssertEqual(guess(got, 64), "y4:3")
    }

    func testNoMatchStillNamesTheClosest() {
        let got = [UInt8](repeating: 0x55, count: 32 * 32 * 4)
        let answer = guess(got, 32)
        XCTAssertTrue(answer.contains(":"), answer)
        XCTAssertFalse(answer.hasSuffix(":0"), answer)
    }
}
