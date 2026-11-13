import CoreMedia
import CoreVideo
import Foundation
import HWTestCore
import VideoToolbox

public struct DecodeResult {
    public let frames: Int
    /// Frames whose barcode named the right frame and whose gradient was within tolerance.
    public let matched: Int
    public let firstMismatch: String?
    public let usingHardware: Bool
    public let decodeSeconds: Double
    public let worstLumaError: Double
    public let worstChromaError: Double
    /// Every picture came out after the ones before it in display order.
    public let displayOrdered: Bool
    /// How the decoder's pictures reached the client (frames, exported, vic, cpu, errors), where it says.
    public var routes: [String: Int]?
    /// The AVD plugin's SwitchDisplayBufferProbe reading (mode2_*, mode3_*), when it was asked for and ran.
    public var displayBufferProbe: [String: Int]?

    public var millisecondsPerFrame: Double { frames > 0 ? decodeSeconds * 1000 / Double(frames) : 0 }
    public var fps: Double { decodeSeconds > 0 ? Double(frames) / decodeSeconds : 0 }
}

public enum Decompressor {
    public static let tolerance = 12.0

    /// Decodes `samples` (in decode order) on the hardware decoder and checks every frame against the pattern. A
    /// missing hardware decoder throws: there is no software fallback to measure by mistake. Only the media tool's check
    /// on the Mac lets go of the hardware (`requireHardware: false`). `temporal` asks the decoder for display order.
    public static func decode(
        _ samples: [CMSampleBuffer], format: CMVideoFormatDescription, pattern: Pattern, bitDepth: Int, fps: Int,
        requireHardware: Bool = true, probeDisplayBuffer: Bool = false, temporal: Bool = false
    ) throws -> DecodeResult {
        let session = try makeSession(
            format: format, width: pattern.width, height: pattern.height, bitDepth: bitDepth,
            requireHardware: requireHardware)
        defer { VTDecompressionSessionInvalidate(session) }
        if probeDisplayBuffer {
            VTSessionSetProperty(session, key: "SwitchDisplayBufferProbe" as CFString, value: kCFBooleanTrue)
        }
        let collector = Collector(pattern: pattern, fps: fps)
        let start = CFAbsoluteTimeGetCurrent()
        var flags: VTDecodeFrameFlags = [._EnableAsynchronousDecompression]
        if temporal { flags.insert(._EnableTemporalProcessing) }
        for (index, sample) in samples.enumerated() {
            let status = VTDecompressionSessionDecodeFrame(
                session, sampleBuffer: sample, flags: flags, infoFlagsOut: nil
            ) { status, _, image, pts, _ in collector.add(status, image, pts) }
            guard status == noErr else { throw VideoError.status("VTDecompressionSessionDecodeFrame \(index)", status) }
        }
        if temporal { VTDecompressionSessionFinishDelayedFrames(session) }
        let waited = VTDecompressionSessionWaitForAsynchronousFrames(session)
        let wall = CFAbsoluteTimeGetCurrent() - start
        guard waited == noErr else {
            throw VideoError.status("VTDecompressionSessionWaitForAsynchronousFrames", waited)
        }
        var result = collector.result(
            expected: samples.count, seconds: max(0, wall - collector.checkSeconds),
            usingHardware: Compressor.readFlag(
                session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder))
        result.routes = pictureRoutes(session)
        if probeDisplayBuffer {
            result.displayBufferProbe =
                (Compressor.read(session, "SwitchDisplayBufferProbe" as CFString)
                as? [String: NSNumber])?.mapValues(\.intValue)
        }
        return result
    }

    /// Decodes `samples` (in decode order) on the hardware decoder and keeps its pictures, in display order, untouched
    /// by the CPU (a CPU read would turn a native NVDEC picture into its pitch rows), with the decoder's routes.
    public static func decodePictures(
        _ samples: [CMSampleBuffer], format: CMVideoFormatDescription, width: Int, height: Int, bitDepth: Int
    ) throws -> (pictures: [CVPixelBuffer], routes: [String: Int]?) {
        let session = try makeSession(
            format: format, width: width, height: height, bitDepth: bitDepth, requireHardware: true)
        defer { VTDecompressionSessionInvalidate(session) }
        let kept = Pictures()
        for (index, sample) in samples.enumerated() {
            let status = VTDecompressionSessionDecodeFrame(
                session, sampleBuffer: sample, flags: [._EnableAsynchronousDecompression], infoFlagsOut: nil
            ) { status, _, image, pts, _ in kept.add(status, image, pts) }
            guard status == noErr else { throw VideoError.status("VTDecompressionSessionDecodeFrame \(index)", status) }
        }
        let waited = VTDecompressionSessionWaitForAsynchronousFrames(session)
        guard waited == noErr else {
            throw VideoError.status("VTDecompressionSessionWaitForAsynchronousFrames", waited)
        }
        return (try kept.inOrder(expected: samples.count), pictureRoutes(session))
    }

    final class Pictures: @unchecked Sendable {
        private let lock = NSLock()
        private var pictures: [(CMTime, CVPixelBuffer)] = []
        private var failure: OSStatus = noErr

        func add(_ status: OSStatus, _ image: CVImageBuffer?, _ pts: CMTime) {
            lock.lock()
            defer { lock.unlock() }
            if status != noErr {
                failure = status
            } else if let image {
                pictures.append((pts, image))
            }
        }

        func inOrder(expected: Int) throws -> [CVPixelBuffer] {
            lock.lock()
            defer { lock.unlock() }
            guard failure == noErr else { throw VideoError.status("decoder output", failure) }
            guard pictures.count == expected else {
                throw VideoError.message("the decoder returned \(pictures.count) of \(expected) pictures")
            }
            return pictures.sorted { $0.0 < $1.0 }.map(\.1)
        }
    }

    /// The Switch decoder's SwitchPictureRoutes (debug builds of the AVD plugin): nil from any other decoder.
    static func pictureRoutes(_ session: VTDecompressionSession) -> [String: Int]? {
        (Compressor.read(session, "SwitchPictureRoutes" as CFString) as? [String: NSNumber])?.mapValues(\.intValue)
    }

    static func makeSession(
        format: CMVideoFormatDescription, width: Int, height: Int, bitDepth: Int, requireHardware: Bool
    ) throws -> VTDecompressionSession {
        let specification: [CFString: Any] = [
            kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: requireHardware
        ]
        let destination = PatternPixels.attributes(
            width: width, height: height, format: PatternPixels.format(bitDepth: bitDepth))
        var session: VTDecompressionSession?
        let status = VTDecompressionSessionCreate(
            allocator: nil, formatDescription: format, decoderSpecification: specification as CFDictionary,
            imageBufferAttributes: destination as CFDictionary, outputCallback: nil, decompressionSessionOut: &session)
        guard status == noErr, let session else { throw VideoError.status("VTDecompressionSessionCreate", status) }
        return session
    }

    final class Collector: @unchecked Sendable {
        private let lock = NSLock()
        private let pattern: Pattern
        private let fps: Int
        private var frames = 0
        private var matched = 0
        private var firstMismatch: String?
        private var worstLuma = 0.0
        private var worstChroma = 0.0
        private var lastFrame = -1
        private var ordered = true
        private(set) var checkSeconds = 0.0

        init(pattern: Pattern, fps: Int) {
            self.pattern = pattern
            self.fps = fps
        }

        func add(_ status: OSStatus, _ image: CVImageBuffer?, _ pts: CMTime) {
            let start = CFAbsoluteTimeGetCurrent()
            let expected = Int((pts.seconds * Double(fps)).rounded())
            var verdict: FrameCheck?
            if status == noErr, let image {
                verdict = PatternPixels.check(image, pattern: pattern, expected: expected)
            }
            lock.lock()
            defer { lock.unlock() }
            frames += 1
            ordered = ordered && expected > lastFrame
            lastFrame = expected
            if let verdict, verdict.matches(frame: expected, tolerance: Decompressor.tolerance) {
                matched += 1
            } else if firstMismatch == nil {
                firstMismatch =
                    verdict.map {
                        "frame \(expected): read \($0.frame.map(String.init) ?? "bad CRC"), luma error "
                            + String(format: "%.1f chroma error %.1f", $0.lumaError, $0.chromaError)
                    } ?? "frame \(expected): decoder status \(status)"
            }
            if let verdict {
                worstLuma = max(worstLuma, verdict.lumaError)
                worstChroma = max(worstChroma, verdict.chromaError)
            }
            checkSeconds += CFAbsoluteTimeGetCurrent() - start
        }

        func result(expected: Int, seconds: Double, usingHardware: Bool) -> DecodeResult {
            lock.lock()
            defer { lock.unlock() }
            let missing = expected - frames
            return DecodeResult(
                frames: frames, matched: matched,
                firstMismatch: firstMismatch ?? (missing > 0 ? "\(missing) frames never came out" : nil),
                usingHardware: usingHardware, decodeSeconds: seconds, worstLumaError: worstLuma,
                worstChromaError: worstChroma, displayOrdered: ordered)
        }
    }
}
