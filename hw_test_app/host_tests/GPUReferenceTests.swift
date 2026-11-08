import Foundation
import HWTestCore
import XCTest

/// The CPU references the GPU scenes compare against, and the diff and pacing summaries their log lines carry.
final class GPUReferenceTests: XCTestCase {
    func testDiffNamesWhereAndHowAPictureIsWrong() {
        let want = Raster(width: 16, height: 8, channels: 1) { _, _, _ in 0.5 }
        var got = want
        for y in 0..<8 { got[3, y, 0] = 0 }
        got[12, 1, 0] = 0.52
        let diff = RasterDiff.compare(got, want, tolerance: 1.01 / 255)
        XCTAssertEqual(diff.wrong, 9)
        XCTAssertEqual(diff.first, [3, 0])
        XCTAssertEqual(diff.firstGot, "0")
        XCTAssertEqual(diff.firstWant, "128")
        XCTAssertEqual(diff.box, [3, 0, 12, 7])
        XCTAssertEqual(diff.streakColumns, 1)
        XCTAssertEqual(diff.map, "4042404040404040")
        XCTAssertEqual(diff.metrics(prefix: "raw_")["raw_wrong"], .integer(9))
        XCTAssertTrue(diff.summary.hasPrefix("9 wrong of 128 at 3,0 got 0 want 128"))
    }

    func testNaNInTheReferenceMatchesAnything() {
        let want = Raster(width: 2, height: 1, channels: 1, samples: [.nan, 0.25])
        let got = Raster(width: 2, height: 1, channels: 1, samples: [0.9, 0.25])
        XCTAssertTrue(RasterDiff.compare(got, want, tolerance: 0).matches)
        XCTAssertFalse(RasterDiff.compare(want, got, tolerance: 0).matches)
    }

    func testBoxDownsamplingAveragesBlocks() {
        let image = Raster(width: 4, height: 4, channels: 1) { x, y, _ in Float((x + y) % 2) }
        XCTAssertEqual(image.boxDownsampled(by: 2).samples, [0.5, 0.5, 0.5, 0.5])
    }

    func testFormatsRoundTripTheirOwnBytes() {
        let value: [Float] = [0.2, 0.4, 0.6, 1]
        for format in TexelFormat.allCases {
            let texel = Array(value.prefix(format.channels))
            let bytes = format.encode(texel)
            XCTAssertEqual(bytes.count, format.bytesPerTexel, format.rawValue)
            let srgb = format == .rgba8Srgb || format == .bgra8Srgb
            let linear = format.decode(bytes[...]).enumerated().map { srgb && $0 < 3 ? TexelFormat.linear($1) : $1 }
            XCTAssertEqual(format.encode(linear), bytes, format.rawValue)
        }
        XCTAssertEqual(TexelFormat.bgra8Unorm.encode([1, 0.5, 0, 1]), [0, 128, 255, 255])
        XCTAssertEqual(TexelFormat.rgba8Srgb.encode([0.2, 0.2, 0.2, 0.2]), [124, 124, 124, 51])
        XCTAssertEqual(
            TexelFormat.bgr10XR.decode(TexelFormat.bgr10XR.encode([1.25, -0.75, 0])[...])[1], -0.749, accuracy: 0.002)
        XCTAssertEqual(TexelFormat.bgra10XR.decode(TexelFormat.bgra10XR.encode([0, 0, 0, 1.3])[...])[3], 1)
        XCTAssertEqual(
            TexelFormat.rgba16Float.decode(TexelFormat.rgba16Float.encode([1.5, -2, 0.25, 65504])[...]),
            [1.5, -2, 0.25, 65504])
    }

    func testSRGBRastersAreComparedAsStored() {
        let stored = TexelFormat.bgra8Srgb.raster([124, 124, 124, 255], width: 1, height: 1)
        XCTAssertEqual(stored.samples, [124 / 255, 124 / 255, 124 / 255, 1])
    }

    func testBlendFollowsMetalsEquation() {
        let source: [Float] = [0.8, 0.4, 0.2, 0.6]
        let destination: [Float] = [0.4, 0.6, 0.8, 0.2]
        let over = Blend.apply(
            BlendState(source: .sourceAlpha, destination: .oneMinusSourceAlpha), source: source, source1: source,
            destination: destination, constant: [0, 0, 0, 0], normalized: true)
        XCTAssertEqual(over[0], 0.8 * 0.6 + 0.4 * 0.4, accuracy: 1e-6)
        XCTAssertEqual(over[3], 0.6 * 0.6 + 0.2 * 0.4, accuracy: 1e-6)
        let saturate = Blend.apply(
            BlendState(source: .sourceAlphaSaturated, destination: .zero), source: source, source1: source,
            destination: destination, constant: [0, 0, 0, 0], normalized: true)
        XCTAssertEqual(saturate[0], 0.8 * 0.6, accuracy: 1e-6)
        XCTAssertEqual(saturate[3], 0.6, accuracy: 1e-6)
        let subtract = Blend.apply(
            BlendState(.subtract, source: .one, destination: .one), source: source, source1: source,
            destination: destination, constant: [0, 0, 0, 0], normalized: true)
        XCTAssertEqual(subtract[2], 0)
        let extended = Blend.apply(
            BlendState(.subtract, source: .one, destination: .one), source: source, source1: source,
            destination: destination, constant: [0, 0, 0, 0], normalized: false)
        XCTAssertEqual(extended[2], -0.6, accuracy: 1e-6)
        XCTAssertEqual(BlendFactor.oneMinusSource1Alpha.rawValue, 18)
        XCTAssertEqual(BlendOperation.max.rawValue, 4)
    }

    func testGaussianKeepsFlatPicturesFlatAndSpreadsASpike() {
        let flat = Raster(width: 9, height: 9, channels: 1) { _, _, _ in 0.5 }
        XCTAssertTrue(Gaussian.blur(flat, sigma: 2).samples.allSatisfy { abs($0 - 0.5) < 1e-6 })
        let spike = Raster(width: 9, height: 9, channels: 1) { x, y, _ in x == 4 && y == 4 ? 1 : 0 }
        let blurred = Gaussian.blur(spike, sigma: 1)
        XCTAssertEqual(blurred.samples.reduce(0, +), 1, accuracy: 1e-5)
        XCTAssertEqual(blurred[3, 4, 0], blurred[5, 4, 0], accuracy: 1e-7)
        XCTAssertLessThan(blurred[4, 4, 0], 1)
    }

    func testPacingCountsMissedFramesAndBucketsThem() {
        let period = 1.0 / 60
        var times = (0..<10).map { 1 + Double($0) * period }
        times += [1 + 12 * period, 1 + 13 * period, 1 + 17 * period, 0]
        let pacing = Pacing(times: times, period: period)
        XCTAssertEqual(pacing.frames, 13)
        XCTAssertEqual(pacing.missed, 2 + 3)
        XCTAssertEqual(pacing.histogramText, "10/0/1/1")
        XCTAssertEqual(pacing.metrics(prefix: "link_")["link_missed"], .integer(5))
    }

    func testPathsAndCountersBecomeLogFields() {
        var tally = PathTally()
        ["raw", "m2v:state:state.notpack", "raw"].forEach { tally.add($0) }
        XCTAssertEqual(tally.text, "raw/2,m2v:state:state.notpack/1")
        XCTAssertFalse(tally.onlyM2V)
        let metrics = PathTally.counterMetrics(
            before: ["raw": 5, "m2v": 1, "pred.state.notpack": 1],
            after: ["raw": 9, "m2v": 4, "pred.state.notpack": 3, "pred.compute": 1])
        XCTAssertEqual(metrics["raw_cbs"], .integer(4))
        XCTAssertEqual(metrics["m2v_cbs"], .integer(3))
        XCTAssertEqual(metrics["m2v_why"], .text("state.notpack/2,compute/1"))
    }

    func testFittedReasonsKeepTheFirstAndCountTheRest() {
        XCTAssertEqual(LogLine.fitted(["a", "bb", "ccc"], budget: 10), "a; bb; ccc")
        XCTAssertEqual(
            LogLine.fitted([String(repeating: "x", count: 12), "y", "z"], budget: 10), "xxxxxxxxxxxx; +2 more")
        XCTAssertEqual(LogLine.fitted([]), "")
    }

    func testPipelineLinesParseAsTheCorpusWritesThem() throws {
        let line = try XCTUnwrap(
            PipelineLine(
                "R ok=1 label=com.apple.coreanimation.PBGRAXm_A2Xhf samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 "
                    + "c1=80:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a3=27:32:1:48:1:1 | v fixed_vert_lph_spc "
                    + "lib=/System/Library/Frameworks/QuartzCore.framework/default.metallib fc_generic:53:00 | "
                    + "f fixed_frag_lph_cph fc_color_opcodes0:52:0a0b0c0d fc_blend_function:49:3a |"))
        XCTAssertEqual(line.shortLabel, "PBGRAXm_A2Xhf")
        XCTAssertEqual(line.colors.map(\.format), [80, 80])
        XCTAssertEqual(line.colors[1].writeMask, 0)
        XCTAssertEqual(line.attributes.map(\.index), [0, 3])
        XCTAssertEqual(line.vertex.name, "fixed_vert_lph_spc")
        XCTAssertEqual(line.vertex.constants.map(\.name), ["fc_generic"])
        XCTAssertEqual(line.vertex.constants.first?.type, 53)
        XCTAssertEqual(line.vertex.constants.first?.bytes, [0])
        XCTAssertEqual(line.fragment.constants.first?.bytes, [0x0a, 0x0b, 0x0c, 0x0d])
        XCTAssertNil(PipelineLine("R ok=1 c0=80:15 | v a | f b |"))
    }

    func testBlurredStripesAreSmoothAndConstantDownEachColumn() {
        let blurred = Raster(width: 32, height: 8, channels: 3) { x, _, _ in 0.5 + 0.1 * sin(Float(x) / 3) }
        let stripes = Backdrop.overStripes(blurred)
        XCTAssertEqual(stripes.columnSpread, 0)
        XCTAssertLessThan(stripes.step, 0.05)
        XCTAssertGreaterThan(stripes.contrast, 0.15)
        var streaked = blurred
        for y in 0..<8 where y % 2 == 0 { for c in 0..<3 { streaked[7, y, c] = 0.1 } }
        XCTAssertGreaterThan(Backdrop.overStripes(streaked).columnSpread, 0.3)
        XCTAssertEqual(Backdrop.overUniform(Raster(width: 2, height: 2, channels: 1) { x, _, _ in Float(x) }).spread, 1)
    }
}
