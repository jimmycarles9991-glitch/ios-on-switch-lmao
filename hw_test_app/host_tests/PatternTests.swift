import Foundation
import HWTestCore
import XCTest

/// The SDK has a Pattern of its own; the tests mean the app's.
typealias Pattern = HWTestCore.Pattern

/// An in-memory 4:2:0 picture the writer fills and the checker reads, like a decoded CVPixelBuffer.
struct Picture {
    let pattern: Pattern
    let sampleBytes: Int
    var luma: [UInt8]
    var chroma: [UInt8]

    init(_ pattern: Pattern, frame: Int, sampleBytes: Int = 1) {
        self.pattern = pattern
        self.sampleBytes = sampleBytes
        luma = [UInt8](repeating: 0, count: pattern.width * pattern.height * sampleBytes)
        chroma = [UInt8](repeating: 0, count: pattern.width * pattern.height / 2 * sampleBytes)
        let rowBytes = pattern.width * sampleBytes
        luma.withUnsafeMutableBytes {
            PatternWriter.writeLuma(
                pattern, frame: frame,
                into: .init(base: $0.baseAddress!, rowBytes: rowBytes, sampleBytes: sampleBytes))
        }
        chroma.withUnsafeMutableBytes {
            PatternWriter.writeChromaInterleaved(
                pattern, into: .init(base: $0.baseAddress!, rowBytes: rowBytes, sampleBytes: sampleBytes))
        }
    }

    func sample(_ plane: [UInt8], _ index: Int) -> Double {
        if sampleBytes == 1 { return Double(plane[index]) }
        return Double(UInt16(plane[index * 2]) | UInt16(plane[index * 2 + 1]) << 8) / 256
    }

    func meanLuma(_ region: Region) -> Double {
        var sum = 0.0
        for y in region.y..<(region.y + region.height) {
            for x in region.x..<(region.x + region.width) { sum += sample(luma, y * pattern.width + x) }
        }
        return sum / Double(region.width * region.height)
    }

    func meanChroma(_ region: Region) -> (cb: Double, cr: Double) {
        var (cb, cr, count) = (0.0, 0.0, 0.0)
        for cy in (region.y / 2)..<((region.y + region.height) / 2) {
            for cx in (region.x / 2)..<((region.x + region.width) / 2) {
                cb += sample(chroma, cy * pattern.width + cx * 2)
                cr += sample(chroma, cy * pattern.width + cx * 2 + 1)
                count += 1
            }
        }
        return (cb / count, cr / count)
    }

    func check(expected: Int) -> FrameCheck {
        PatternChecker(pattern).check(expected: expected, luma: meanLuma, chroma: meanChroma)
    }
}

final class PatternTests: XCTestCase {
    func testCRCMatchesTheStandardCheckValue() {
        XCTAssertEqual(CRC16.ccitt(Array("123456789".utf8)), 0x29B1)
    }

    func testNV12RoundTripRecoversTheFrameExactly() {
        let pattern = Pattern(width: 320, height: 192, clip: 0x0102)
        for frame in [0, 1, 37, 59, 65535] {
            let result = Picture(pattern, frame: frame).check(expected: frame)
            XCTAssertEqual(result.frame, frame)
            XCTAssertTrue(result.matches(frame: frame, tolerance: 1), "\(result)")
        }
    }

    func testP010CarriesTheSameValues() {
        let pattern = Pattern(width: 256, height: 128, clip: 7)
        let result = Picture(pattern, frame: 12, sampleBytes: 2).check(expected: 12)
        XCTAssertTrue(result.matches(frame: 12, tolerance: 1), "\(result)")
    }

    func testAFrameOutOfOrderReadsAsItsOwnNumber() {
        let pattern = Pattern(width: 256, height: 128, clip: 7)
        let result = Picture(pattern, frame: 5).check(expected: 4)
        XCTAssertEqual(result.frame, 5)
        XCTAssertFalse(result.matches(frame: 4, tolerance: 12))
    }

    func testAnotherClipsFrameFailsItsCRC() {
        let theirs = Picture(Pattern(width: 256, height: 128, clip: 8), frame: 3)
        let result = PatternChecker(Pattern(width: 256, height: 128, clip: 7))
            .check(expected: 3, luma: theirs.meanLuma, chroma: theirs.meanChroma)
        XCTAssertNil(result.frame)
    }

    func testSwappedChromaPlanesFailTheGradient() {
        let picture = Picture(Pattern(width: 256, height: 128, clip: 7), frame: 2)
        let result = PatternChecker(picture.pattern).check(
            expected: 2, luma: picture.meanLuma,
            chroma: {
                let (cb, cr) = picture.meanChroma($0); return (cr, cb)
            })
        XCTAssertEqual(result.frame, 2)
        XCTAssertGreaterThan(result.chromaError, 12)
    }

    func testCodecNoiseWithinToleranceStillPasses() {
        let picture = Picture(Pattern(width: 256, height: 128, clip: 7), frame: 9)
        let result = PatternChecker(picture.pattern).check(
            expected: 9, luma: { picture.meanLuma($0) + ($0.x % 3 == 0 ? 6 : -6) }, chroma: picture.meanChroma)
        XCTAssertTrue(result.matches(frame: 9, tolerance: 12), "\(result)")
    }

    func testRGBRoundTrip() {
        let pattern = Pattern(width: 512, height: 384, clip: 0x5020)
        var pixels = [UInt8](repeating: 0, count: pattern.width * pattern.height * 4)
        pixels.withUnsafeMutableBytes {
            PatternWriter.writeRGB(
                pattern, into: .init(base: $0.baseAddress!, rowBytes: pattern.width * 4), channels: 4)
        }
        let result = PatternChecker(pattern).checkRGB { region in
            var sum = (0.0, 0.0, 0.0)
            for y in region.y..<(region.y + region.height) {
                for x in region.x..<(region.x + region.width) {
                    let at = (y * pattern.width + x) * 4
                    sum = (sum.0 + Double(pixels[at]), sum.1 + Double(pixels[at + 1]), sum.2 + Double(pixels[at + 2]))
                }
            }
            let count = Double(region.width * region.height)
            return (sum.0 / count, sum.1 / count, sum.2 / count)
        }
        XCTAssertTrue(result.matches(frame: 0, tolerance: 0.5), "\(result)")
    }

    func testTheBuildPinsExactlyTheTablesMedia() {
        let listed = (ProcessInfo.processInfo.environment["HWTEST_MEDIA"] ?? "").split(separator: " ").map(String.init)
        XCTAssertEqual(Set(listed), Set(Clip.all.map(\.file) + [AlphaClip.file] + StillImage.all.map(\.file)))
        XCTAssertEqual(listed.count, Set(listed).count)
    }

    func testEveryClipAndImageHasItsOwnSaltAndFile() {
        XCTAssertEqual(Set(Clip.all.map(\.clipID) + [AlphaClip.pattern.clip]).count, Clip.all.count + 1)
        XCTAssertEqual(Set(Clip.all.map(\.file)).count, Clip.all.count)
        XCTAssertEqual(Set(StillImage.all.map(\.file)).count, StillImage.all.count)
        XCTAssertEqual(Clip.named("vp9_4k.ivf")?.height, 2160)
        XCTAssertEqual(Clip.named("hevc_main10_1080p.mp4")?.codec.bitDepth, 10)
    }
}

final class SupportTests: XCTestCase {
    func testIVFRoundTrip() throws {
        let header = IVF.Header(fourcc: "VP90", width: 1280, height: 720, rate: 60, scale: 1, frameCount: 2)
        let frames = [IVF.Frame(pts: 0, data: Data([1, 2, 3])), IVF.Frame(pts: 1, data: Data())]
        let data = IVF.write(header, frames: frames)
        XCTAssertEqual(data.count, 32 + 12 + 3 + 12)
        let (parsed, parsedFrames) = try IVF.parse(data)
        XCTAssertEqual(parsed, header)
        XCTAssertEqual(parsedFrames, frames)
        XCTAssertThrowsError(try IVF.parse(data.prefix(40)))
        XCTAssertThrowsError(try IVF.parse(Data("RIFF".utf8) + data.dropFirst(4)))
    }

    func testPercentilesAndDroppedFrames() {
        let values = (1...100).map(Double.init)
        XCTAssertEqual(Stats.percentile(values, 0.5), 50)
        XCTAssertEqual(Stats.percentile(values, 0.99), 99)
        XCTAssertEqual(Stats.percentile([], 0.5), 0)
        let period = 1.0 / 60
        XCTAssertEqual(
            Stats.droppedFrames(intervals: [period, period * 2, period * 1.02, period * 3.1], period: period), 3)
    }
}
