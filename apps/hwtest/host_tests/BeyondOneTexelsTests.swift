import Foundation
import HWTestCore
import XCTest

/// gpu.gm20b.resolve_twod's report of texels beyond one code of Metal's resolve
final class BeyondOneTexelsTests: XCTestCase {
    func testOneCodeOffIsLeftOutAndTwoIsKept() {
        var texels = BeyondOneTexels()
        texels.add(x: 0, y: 0, got: [10, 20, 30, 255], want: [11, 19, 30, 255], samples: [[10, 20, 30, 255]])
        XCTAssertEqual(texels.count, 0)
        texels.add(x: 3, y: 6, got: [10, 20, 30, 255], want: [12, 20, 30, 255], samples: [[0, 1, 2, 3], [4, 5, 6, 7]])
        XCTAssertEqual(texels.count, 1)
        XCTAssertEqual(texels.text, "1|3,6:a.14.1e.ff~c.14.1e.ff@0.1.2.3/4.5.6.7")
    }

    func testOnlyTheFirstShownAreListedAndAllCounted() {
        var texels = BeyondOneTexels(shown: 2)
        for x in 0 ..< 5 {
            texels.add(x: x, y: 0, got: [0], want: [2], samples: [[0], [4]])
        }
        XCTAssertEqual(texels.count, 5)
        XCTAssertEqual(texels.text, "5|0,0:0~2@0/4|1,0:0~2@0/4")
    }

    func testBgra8ReadsRgbaCodesAndTheScenesSamples() {
        let width = 2, samples = 2
        var want = [UInt8](repeating: 0, count: width * 4)
        for t in 0 ..< width {
            for c in 0 ..< 4 {
                let sum = (0 ..< samples).map { Int(ResolveRef.sampleHash(t, 0, $0) >> (8 * c) & 255) }.reduce(0, +)
                want[4 * t + [2, 1, 0, 3][c]] = UInt8((sum + samples / 2) / samples)
            }
        }
        XCTAssertEqual(BeyondOneTexels.bgra8(want, want, width: width, samples: samples).count, 0)
        var got = want
        got[4 + 2] = want[4 + 2] &+ 2
        let texels = BeyondOneTexels.bgra8(got, want, width: width, samples: samples)
        XCTAssertEqual(texels.count, 1)
        let h = (0 ..< samples).map { ResolveRef.sampleHash(1, 0, $0) }
        let sampleText = h.map { s in (0 ..< 4).map { String(s >> (8 * $0) & 255, radix: 16) }.joined(separator: ".") }
        let red = String(UInt32(got[6]), radix: 16), wantRed = String(UInt32(want[6]), radix: 16)
        let rest = [want[5], want[4], want[7]].map { String(UInt32($0), radix: 16) }.joined(separator: ".")
        XCTAssertEqual(
            texels.text, "1|1,0:\(red).\(rest)~\(wantRed).\(rest)@" + sampleText.joined(separator: "/")
        )
    }
}
