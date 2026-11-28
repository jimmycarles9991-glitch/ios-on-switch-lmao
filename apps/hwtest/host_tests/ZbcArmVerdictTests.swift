import HWTestCore
import XCTest

/// The 11-22 to 11-25 runs: every colour arm read `0:0:0` (no texel off, no hit) with the GPU's ZBC colour table full
/// before the arms ran, and passed from 11-24 on
final class ZbcArmVerdictTests: XCTestCase {
    func testAnArmWhoseEntryTheFullTableRefusedFails() {
        XCTAssertEqual(
            ZbcArmVerdict.wrong("rgba32f", firstOff: 0, lastOff: 0, hits: 0, refused: 1), ["zbc_rgba32f_table_full"]
        )
    }

    func testAnArmThatAskedNothingAndHitNothingFails() {
        XCTAssertEqual(
            ZbcArmVerdict.wrong("rgb10a2", firstOff: 0, lastOff: 0, hits: 0, refused: 0), ["zbc_rgb10a2_no_hit"]
        )
    }

    func testAnArmThatHitItsEntryWithItsTexelsPasses() {
        XCTAssertEqual(ZbcArmVerdict.wrong("depth16", firstOff: 0, lastOff: 0, hits: 12, refused: 0), [])
    }

    func testTexelsOffFailEvenWithAHit() {
        XCTAssertEqual(
            ZbcArmVerdict.wrong("bgra8srgb", firstOff: 1, lastOff: 2, hits: 9, refused: 0),
            ["zbc_bgra8srgb_first", "zbc_bgra8srgb_held_texel"]
        )
    }
}
