import Foundation
import HWTestCore
import XCTest

/// The references gpu.x1.blend_exact and gpu.x1.rop_precision hold the GPU to, against what the device read (the first
/// misses of blend_exact on the boots of 11 Nov, tools/benchloop/blend_models_test.py) and what Metal gives.
final class X1ReferenceTests: XCTestCase {
    typealias Factor = X1Reference.Factor

    static let pairs: [String: (Factor, Factor)] = [
        "sov": (.one, .oneMinusSa), "dov": (.oneMinusDa, .one), "sin": (.da, .zero), "din": (.zero, .sa),
    ]
    static let cases: [(Int, Int)] = (0 ..< 256).flatMap { a in (0 ... a).map { ($0, a) } }

    /// The probe's inputs at pixel.channel: halves nearest k / 255, destination codes (255 - d, 7d + 3, d, d).
    func inputs(_ pixel: Int, _ channel: Int) -> (s: Double, sa: Double, dst: Int, da: Int) {
        let (si, ai) = Self.cases[pixel >> 8]
        let d = pixel & 255
        let s = X1Reference.half(Double(si) / 255), sa = X1Reference.half(Double(ai) / 255)
        return (channel == 3 ? sa : s, sa, [255 - d, (d * 7 + 3) & 255, d, d][channel], d)
    }

    func chip(_ s: Double, _ sa: Double, _ dst: Int, _ da: Int, _ pair: (Factor, Factor)) -> Int {
        let s8 = X1Reference.store(s), sa8 = X1Reference.store(sa)
        return X1Reference.chipBlend(s8, pair.0.code(sa: sa8, da: da), dst, pair.1.code(sa: sa8, da: da))
    }

    func metal(_ s: Double, _ sa: Double, _ dst: Int, _ da: Int, _ pair: (Factor, Factor)) -> Int {
        let daTexel = X1Reference.texel(da, top: 255)
        let fs = pair.0.value(sa: sa, da: daTexel), fd = pair.1.value(sa: sa, da: daTexel)
        return X1Reference.unorm(X1Reference.halfSeparate(s, fs, X1Reference.texel(dst, top: 255), fd), top: 255)
    }

    func testTheChipModelGivesWhatTheDeviceReadWhereMetalsHalfArithmeticDiffers() {
        for (pair, pixel, channel, device, apple) in [
            ("sov", 347, 1, 127, 128), ("dov", 624, 0, 144, 143), ("sin", 35063, 3, 15, 16), ("din", 34824, 0, 15, 16),
        ] {
            let (s, sa, dst, da) = inputs(pixel, channel)
            XCTAssertEqual(chip(s, sa, dst, da, Self.pairs[pair]!), device, pair)
            XCTAssertEqual(metal(s, sa, dst, da, Self.pairs[pair]!), apple, pair)
        }
    }

    func testTheChipRoundsTheSumOnceWhereRoundingEachProductMisses() {
        let s8 = X1Reference.store(X1Reference.half(1 / 255)), sa8 = X1Reference.store(X1Reference.half(200 / 255))
        let (fs, fd) = (Factor.da.code(sa: sa8, da: 12), Factor.oneMinusSa.code(sa: sa8, da: 12))
        XCTAssertEqual(X1Reference.chipBlend(s8, fs, 183, fd), 40)
        XCTAssertEqual(X1Reference.perProductBlend(s8, fs, 183, fd), 39)
    }

    func testTheStoreTruncatesToFourMoreBitsThenRoundsTiesDown() {
        XCTAssertEqual(X1Reference.store(0.5), 127)
        XCTAssertEqual(X1Reference.store(0.25), 64)
        XCTAssertEqual(X1Reference.store(1.0), 255)
        XCTAssertEqual(X1Reference.store(.nan), 0)
        XCTAssertEqual((0 ..< 6).map { X1Reference.store((Double($0) + 0.5) / 255) }, Array(0 ..< 6))
        XCTAssertEqual(X1Reference.unorm(0.5, top: 255), 128, "Metal's store rounds the same tie away from zero")
    }
}
