import Foundation
import HWTestCore
import XCTest

final class LogPiecesTests: XCTestCase {
    func assertWhole(_ line: String, file: StaticString = #filePath, lineNumber: UInt = #line) {
        let pieces = LogLine.pieces(line)
        XCTAssertGreaterThan(pieces.count, 1, file: file, line: lineNumber)
        for (index, piece) in pieces.enumerated() {
            XCTAssertLessThanOrEqual(piece.utf8.count, LogLine.pieceBytes, file: file, line: lineNumber)
            if index > 0 {
                XCTAssertTrue(piece.hasPrefix("HWTEST ui.scale_pacing +\(index) "), piece, file: file, line: lineNumber)
            }
        }
        XCTAssertEqual(LogLine.joined(pieces), line, file: file, line: lineNumber)
    }

    func testALongLineReachesTheLogWholeInPiecesThatEndOnFields() {
        var metrics = Metrics()
        for round in 0 ..< 120 { metrics.set("open\(round)_shown", round * 7) }
        let line = LogLine.format(id: "ui.scale_pacing", outcome: .fail("zoom 3 missed", metrics), milliseconds: 20200)
        assertWhole(line)
        let pieces = LogLine.pieces(line)
        XCTAssertEqual(LogLine.parse(pieces[0])?.verdict, .fail)
        XCTAssertNil(LogLine.parse(pieces[1]), "a continuation is no test line of its own")
        XCTAssertFalse(pieces[0].hasSuffix(" "))
        XCTAssertTrue(pieces[1].hasPrefix("HWTEST ui.scale_pacing +1  "), "it starts with the space it was cut before")
    }

    /// A run of three-byte characters behind 0, 1 and 2 bytes of padding: some piece ends inside a character unless
    /// the cut steps back to its first byte.
    func testAFieldLongerThanAPieceIsCutBetweenCharacters() {
        for pad in 0 ..< 3 {
            var metrics = Metrics()
            metrics.set("glyphs", String(repeating: "x", count: pad) + String(repeating: "–", count: 1000))
            assertWhole(LogLine.format(id: "ui.scale_pacing", outcome: .pass(metrics), milliseconds: 1))
        }
        let why = String(repeating: "naïve–cut ", count: 300)
        assertWhole(LogLine.format(id: "ui.scale_pacing", outcome: .fail(why), milliseconds: 1))
    }

    func testALineThatFitsIsOnePiece() {
        let line = "HWTEST cpu.single PASS ms=12.3"
        XCTAssertEqual(LogLine.pieces(line), [line])
        XCTAssertEqual(LogLine.joined([line]), line)
    }
}
