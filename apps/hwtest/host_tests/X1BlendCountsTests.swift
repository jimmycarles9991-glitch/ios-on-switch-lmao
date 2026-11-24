import Foundation
import HWTestCore
import XCTest

final class X1BlendCountsTests: XCTestCase {
    typealias Factor = X1Reference.Factor
    typealias Counts = X1Reference.BlendCounts

    static let pixels = X1Reference.blendCases.count * 256
    static let halves = (0 ..< 256).map { Double(Float16(Double($0) / 255)) }

    /// The loop gpu.x1.blend_exact and gpu.x1.rop_precision counted with before (X1BlendProbes.compare and
    /// Counts.add): every channel, every model, one after the other.
    static func loop(_ bytes: [UInt8], _ source: Factor, _ destination: Factor) -> Counts {
        var counts = Counts()
        let texels = (0 ..< 256).map { X1Reference.texel($0, top: 255) }
        func add(_ have: Int, _ s: Double, _ sa: Double, _ dc: Int, _ da: Int, p: Int, channel: Int) {
            let texel = texels[dc], daTexel = texels[da]
            let fs = source.value(sa: sa, da: daTexel), fd = destination.value(sa: sa, da: daTexel)
            let want = X1Reference.unorm(X1Reference.halfSeparate(s, fs, texel, fd), top: 255)
            if have != want {
                counts.separate += 1
                if have > want { counts.up += 1 } else { counts.down += 1 }
                if counts.first == nil { counts.first = "\(p).\(channel):\(have),want_\(want)" }
            }
            if have != X1Reference.unorm(X1Reference.halfFMA(s, fs, texel, fd), top: 255) { counts.fma += 1 }
            if have != X1Reference.unorm(X1Reference.exact(s, fs, texel, fd), top: 255) { counts.exact += 1 }
            let s8 = X1Reference.store(s), sa8 = X1Reference.store(sa)
            let f1 = source.code(sa: sa8, da: da), f2 = destination.code(sa: sa8, da: da)
            if have != X1Reference.chipBlend(s8, f1, dc, f2) { counts.chip += 1 }
            if have != X1Reference.perProductBlend(s8, f1, dc, f2) { counts.perProduct += 1 }
            let real = s * source.exact(sa: sa, da: da) + Double(dc) / 255 * destination.exact(sa: sa, da: da)
            if have != X1Reference.unorm(real, top: 255) { counts.real += 1 }
            counts.maxDiff = max(counts.maxDiff, abs(have - want))
        }
        for p in 0 ..< pixels {
            let (si, ai) = X1Reference.blendCases[p >> 8]
            let (s, sa) = (halves[Int(si)], halves[Int(ai)])
            let (b, g, r, a) = X1Reference.blendDestination(p)
            add(Int(bytes[p * 4]), s, sa, b, a, p: p, channel: 0)
            add(Int(bytes[p * 4 + 1]), s, sa, g, a, p: p, channel: 1)
            add(Int(bytes[p * 4 + 2]), s, sa, r, a, p: p, channel: 2)
            add(Int(bytes[p * 4 + 3]), sa, sa, a, a, p: p, channel: 3)
        }
        return counts
    }

    /// The chip's blend in every channel: what the device wrote for blend_exact (model=chip, chip_misses=0).
    static func chipImage(_ source: Factor, _ destination: Factor) -> [UInt8] {
        var image = [UInt8](repeating: 0, count: pixels * 4)
        for p in 0 ..< pixels {
            let (si, ai) = X1Reference.blendCases[p >> 8]
            let s8 = X1Reference.store(halves[Int(si)]), sa8 = X1Reference.store(halves[Int(ai)])
            let (b, g, r, a) = X1Reference.blendDestination(p)
            let f1 = source.code(sa: sa8, da: a), f2 = destination.code(sa: sa8, da: a)
            for (channel, (s, dc)) in [(s8, b), (s8, g), (s8, r), (sa8, a)].enumerated() {
                image[p * 4 + channel] = UInt8(X1Reference.chipBlend(s, f1, dc, f2))
            }
        }
        return image
    }

    static func counts(_ image: [UInt8], _ pair: (Factor, Factor), _ models: X1Reference.BlendModels = .all) -> Counts {
        image.withUnsafeBytes { X1Reference.blendCounts($0, source: pair.0, destination: pair.1, models: models) }
    }

    func testTheCountsAreTheLoopsOnTheImageTheDeviceWroteAndAWrongTexelIsCounted() {
        let sov = (Factor.one, Factor.oneMinusSa)
        var image = Self.chipImage(sov.0, sov.1)
        let counts = Self.counts(image, sov)
        XCTAssertEqual(counts, Self.loop(image, sov.0, sov.1))
        XCTAssertEqual(counts.chip, 0)
        XCTAssertEqual(counts.first, "347.1:127,want_128", "the device's first miss of sov")
        XCTAssertGreaterThan(counts.separate, 0)
        image[4_000_001 * 4 + 2] ^= 4
        XCTAssertEqual(Self.counts(image, sov).chip, 1, "one texel off the chip's blend")
    }

    func testTheCountsAreTheLoopsOnBytesNoModelMakes() {
        var state: UInt32 = 0x9E37_79B9
        let noise = (0 ..< Self.pixels * 4).map { _ -> UInt8 in
            state = state &* 1_664_525 &+ 1_013_904_223
            return UInt8(state >> 24)
        }
        let xor = (Factor.oneMinusDa, Factor.oneMinusSa)
        XCTAssertEqual(Self.counts(noise, xor), Self.loop(noise, xor.0, xor.1))
    }

    func testChipAndSeparateCountsThoseTwoAsTheFullCountDoesAndNoOther() {
        let dov = (Factor.oneMinusDa, Factor.one)
        let image = Self.chipImage(.da, .oneMinusSa)
        let full = Self.counts(image, dov), two = Self.counts(image, dov, .chipAndSeparate)
        XCTAssertEqual(
            [two.chip, two.separate, two.up, two.down, two.maxDiff],
            [full.chip, full.separate, full.up, full.down, full.maxDiff]
        )
        XCTAssertEqual(two.first, full.first)
        XCTAssertEqual([two.fma, two.exact, two.perProduct, two.real], [0, 0, 0, 0])
        XCTAssertGreaterThan(full.chip, 0, "a sat image under dov's factors")
    }
}
