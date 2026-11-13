import Foundation
import HWTestCore
import XCTest

/// gpu.astc.layouts' measurement: its blocks name themselves, its block-linear addresses are TegraGM20BLayout.h's, and its
/// bit mapping reads ui.compressed_image's swizzle the way the investigation wrote it down.
final class AstcBlockGridTests: XCTestCase {
    let grid = AstcBlockGrid(across: 32, down: 32)

    func testEveryBlockDecodesToTheBlockItCameFrom() throws {
        let bytes = grid.bytes
        XCTAssertEqual(bytes.count, 32 * 32 * 16)
        for y in 0..<grid.down {
            for x in 0..<grid.across {
                let at = (y * grid.across + x) * 16
                let texel = try XCTUnwrap(AstcBlockGrid.decode(bytes[at..<(at + 16)]))
                let found = grid.source(of: texel)
                XCTAssertEqual(found?.x, x)
                XCTAssertEqual(found?.y, y)
            }
        }
        XCTAssertNil(grid.source(of: SIMD4(0, 0, 0, 0)))
        XCTAssertNil(grid.source(of: SIMD4(.infinity, 0.5, 0.5, 1)))
        XCTAssertNil(grid.source(of: SIMD4(.nan, .nan, .nan, .nan)))
        XCTAssertNil(grid.source(of: SIMD4(1e30, 0.5, 0.5, 1)))
        XCTAssertNil(AstcBlockGrid.decode([UInt8](repeating: 0, count: 16)[...]))
    }

    /// A 128x128 ASTC 4x4 image is 32x32 blocks, 4 GOBs tall (raw/fx blockChainLayout's clamp), 2 KiB a GOB column.
    func testAddressesAreTegraColorGobsInFourGobBlocks() {
        let yLog2 = BlockLinearMap.yLog2(texelHeight: 128, rows: 32)
        XCTAssertEqual(yLog2, 2)
        let at = { (x: Int, y: Int) in BlockLinearMap.offset(xB: x * 16, y: y, rowBytes: 512, yLog2: yLog2) }
        XCTAssertEqual(at(1, 0), 0x20)
        XCTAssertEqual(at(0, 1), 0x10)
        XCTAssertEqual(at(2, 0), 0x100)
        XCTAssertEqual(at(0, 8), 0x200)
        XCTAssertEqual(at(4, 0), 0x800)
        XCTAssertEqual(at(31, 31), 0x3fff - 15)
        let all = Set((0..<32).flatMap { y in (0..<32).map { at($0, y) } })
        XCTAssertEqual(all.count, 1024)
    }

    /// ui.compressed_image's failure: inside each 32 pixels the 8-pixel groups showed groups [1, 2, 0, 3], in the next 32
    /// [3, 0, 2, 1], rows intact.
    func testTheCompressedImageSwizzleReadsAsTheInvestigationsMapping() {
        let groups = [[1, 2, 0, 3], [3, 0, 2, 1]]
        let at = { (x: Int, y: Int) in BlockLinearMap.offset(xB: x * 16, y: y, rowBytes: 512, yLog2: 2) }
        var pairs: [(displayed: Int, written: Int)] = []
        for y in 0..<32 {
            for x in 0..<32 {
                let source = (x & ~7) | groups[(x >> 3) & 1][(x >> 1) & 3] << 1 | (x & 1)
                pairs.append((at(x, y), at(source, y)))
            }
        }
        XCTAssertEqual(BlockLinearMap.bitMap(pairs, bits: 4...13), "g8=w11^w12,g11=w8^w11^w12^1")
        XCTAssertEqual(BlockLinearMap.bitMap(pairs.map { ($0.displayed, $0.displayed) }, bits: 4...13), "identity")
    }
}
