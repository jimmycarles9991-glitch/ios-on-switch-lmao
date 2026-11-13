import Foundation
import HWTestCore
import XCTest

/// YcbcrStore against bytes an M4 Max's Metal stored (the same texels raw_ycbcr_store_test checks the C++ rule with).
final class YcbcrStoreTests: XCTestCase {
    static let block: [SIMD4<Float>] = [
        [1060967144, 1064460635, 1057066791], [1041835799, 1051208286, 1060668499],
        [1027859266, 1053797362, 1047887546], [1060189453, 3165423067, 1051114670],
    ].map { (bits: [UInt32]) in SIMD4(bits.map(Float.init(bitPattern:)) + [1]) }

    func testCodeTargetsStoreEachBlocksRoundedMeanOfItsTexelsCodes() {
        let eight = YcbcrStore.codes(Self.block, width: 2, height: 2, max: 255, halvedDown: true, source: .drawnFloat)
        XCTAssertEqual(eight.luma, [241, 84, 103, 0])
        XCTAssertEqual(eight.chroma, [114, 104])
        let ten = YcbcrStore.codes(Self.block, width: 2, height: 2, max: 1023, halvedDown: true, source: .drawnFloat)
        XCTAssertEqual(ten.luma, [969, 336, 415, 0])
        XCTAssertEqual(ten.chroma, [458, 416])
        let pairs = YcbcrStore.codes(
            Array(Self.block[0..<2]), width: 2, height: 1, max: 255, halvedDown: false, source: .drawnFloat)
        XCTAssertEqual(pairs.chroma, [157, 113])
    }

    func testFourFourFourChromaIsEachTexelsOwnCodes() {
        let texels = YcbcrStore.codes(
            Self.block, width: 2, height: 2, max: 1023, halvedAcross: false, halvedDown: false, source: .drawnFloat)
        XCTAssertEqual(texels.chroma.count, 8)
        for (i, texel) in Self.block.enumerated() {
            XCTAssertEqual(UInt32(texels.chroma[2 * i]), YcbcrStore.code(texel.z, .drawnFloat, max: 1023))
            XCTAssertEqual(UInt32(texels.chroma[2 * i + 1]), YcbcrStore.code(texel.x, .drawnFloat, max: 1023))
        }
    }

    func testHeldHalvesRoundAsMetalHoldsThem() {
        XCTAssertEqual(YcbcrStore.code(Float(bitPattern: 1_016_107_151), .drawnFloat, max: 255), 5)
        XCTAssertEqual(YcbcrStore.code(Float(bitPattern: 1_043_509_941), .drawnFloat, max: 1023), 178)
        XCTAssertEqual(YcbcrStore.code(Float(bitPattern: 1_043_509_941), .clear, max: 1023), 179)
        XCTAssertEqual(YcbcrStore.code(.nan, .clear, max: 255), 0)
        XCTAssertEqual(YcbcrStore.code(.infinity, .drawnFloat, max: 255), 255)
    }

    func testConvertedFormatsAreMtwovsRule() {
        let video = YcbcrStore.converted(Self.block, width: 2, height: 2, full: false, halvedDown: true)
        XCTAssertEqual(video.luma, [199, 86, 77, 69])
        XCTAssertEqual(video.chroma, [132, 126])
        let full = YcbcrStore.converted(Self.block, width: 2, height: 2, full: true, halvedDown: true)
        XCTAssertEqual(full.luma, [213, 81, 71, 62])
        XCTAssertEqual(full.chroma, [132, 126])
    }

    static func texels(_ bits: [[UInt32]]) -> [SIMD4<Float>] {
        bits.map { SIMD4($0.map(Float.init(bitPattern:)) + [1]) }
    }

    func testTheSrgbEncodeIsTheRuntimesTable() {
        let table = YcbcrSrgb.encode
        XCTAssertEqual(table.reduce(0) { $0 + Int($1) }, 46_292_611)
        XCTAssertEqual(table[2], 30)
        XCTAssertEqual(table[6728], 7511)
        XCTAssertEqual(table[8192], 8192)
    }

    func testTenBitAndFourFourFourConvertedFormatsAreMetalsCodes() {
        let x420 = YcbcrStore.converted(
            Self.texels([
                [1_051_734_016, 1_063_543_909, 1_026_113_563], [1_050_820_701, 1_060_977_984, 1_055_496_193],
                [3_166_704_674, 1_064_262_614, 1_044_364_047], [1_041_186_814, 1_040_351_878, 1_002_488_002],
            ]), width: 2, height: 2, bits: 10, full: false, halvedDown: true)
        XCTAssertEqual(x420.luma, [617, 572, 563, 167])
        XCTAssertEqual(x420.chroma, [359, 336])
        let v444 = YcbcrStore.converted(
            Self.texels([[1_063_418_878, 1_065_352_395, 1_059_178_612]]), width: 1, height: 1, full: false,
            halvedAcross: false, halvedDown: false)
        XCTAssertEqual(v444.luma, [218])
        XCTAssertEqual(v444.chroma, [91, 122])
    }

    func testSrgbFormatsAreMetalsCodes() {
        let xf20 = YcbcrStore.converted(
            Self.texels([
                [1_039_875_795, 1_041_249_518, 1_039_463_671], [1_059_260_608, 1_062_939_647, 959_754_239],
                [1_059_442_689, 1_037_340_673, 1_018_212_526], [1_062_847_452, 1_054_760_959, 1_062_606_211],
            ]), width: 2, height: 2, bits: 10, full: true, halvedDown: true, srgb: true)
        XCTAssertEqual(xf20.luma, [409, 812, 484, 807])
        XCTAssertEqual(xf20.chroma, [369, 604])
        let x422 = YcbcrStore.converted(
            Self.texels([
                [1_014_468_608, 1_051_093_400, 1_063_163_816], [1_030_344_703, 1_058_384_457, 3_129_978_432],
            ]),
            width: 2, height: 1, bits: 10, full: false, halvedDown: false, srgb: true)
        XCTAssertEqual(x422.luma, [503, 539])
        XCTAssertEqual(x422.chroma, [486, 304])
        let f444 = YcbcrStore.converted(
            Self.texels([[1_051_159_002, 1_022_423_280, 1_016_107_156]]), width: 1, height: 1, full: true,
            halvedAcross: false, halvedDown: false, srgb: true)
        XCTAssertEqual(f444.luma, [79])
        XCTAssertEqual(f444.chroma, [104, 183])
    }
}
