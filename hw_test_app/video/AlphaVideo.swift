import CoreMedia
import CoreVideo
import Foundation
import HWTestCore
import VideoToolbox

/// AlphaClip's pictures in 'v0a8' buffers (Y and CbCr as '420v', then a full-range alpha plane): written for the Mac's
/// HEVC-with-alpha encoder, read back from VideoToolbox's two-layer decoder.
public enum AlphaVideo {
    public static let format = kCVPixelFormatType_420YpCbCr8VideoRange_8A_TriPlanar

    /// The Mac's encoder with frame reordering on, the one IDR first.
    public static func encode() throws -> CompressedClip {
        var made: VTCompressionSession?
        let created = VTCompressionSessionCreate(
            allocator: nil, width: Int32(AlphaClip.width), height: Int32(AlphaClip.height),
            codecType: kCMVideoCodecType_HEVCWithAlpha, encoderSpecification: nil, imageBufferAttributes: nil,
            compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &made)
        guard created == noErr, let session = made else {
            throw VideoError.status("VTCompressionSessionCreate", created)
        }
        defer { VTCompressionSessionInvalidate(session) }
        let properties: [CFString: Any] = [
            kVTCompressionPropertyKey_RealTime: false,
            kVTCompressionPropertyKey_AllowFrameReordering: true,
            kVTCompressionPropertyKey_AverageBitRate: AlphaClip.bitrate,
            kVTCompressionPropertyKey_ExpectedFrameRate: AlphaClip.fps,
            kVTCompressionPropertyKey_MaxKeyFrameInterval: 100_000,
            kVTCompressionPropertyKey_AlphaChannelMode: kVTAlphaChannelMode_StraightAlpha,
            kVTCompressionPropertyKey_TargetQualityForAlpha: 0.9,
        ]
        for (key, value) in properties {
            let set = VTSessionSetProperty(session, key: key, value: value as CFTypeRef)
            guard set == noErr else { throw VideoError.status("VTSessionSetProperty \(key)", set) }
        }
        let pool = try PatternPixels.pool(width: AlphaClip.width, height: AlphaClip.height, format: format)
        let collector = DecodeOrder()
        let start = CFAbsoluteTimeGetCurrent()
        for frame in 0..<AlphaClip.frames {
            var input: CVPixelBuffer?
            let status = CVPixelBufferPoolCreatePixelBuffer(nil, pool, &input)
            guard status == kCVReturnSuccess, let input else { throw VideoError.status("CVPixelBufferPool", status) }
            fill(input, frame: frame)
            let encoded = VTCompressionSessionEncodeFrame(
                session, imageBuffer: input,
                presentationTimeStamp: CMTime(value: CMTimeValue(frame), timescale: CMTimeScale(AlphaClip.fps)),
                duration: CMTime(value: 1, timescale: CMTimeScale(AlphaClip.fps)), frameProperties: nil,
                infoFlagsOut: nil
            ) { status, _, sample in collector.add(status, sample) }
            guard encoded == noErr else { throw VideoError.status("VTCompressionSessionEncodeFrame \(frame)", encoded) }
        }
        let flushed = VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
        guard flushed == noErr else { throw VideoError.status("VTCompressionSessionCompleteFrames", flushed) }
        let samples = try collector.samples(expected: AlphaClip.frames)
        guard let description = CMSampleBufferGetFormatDescription(samples[0]) else {
            throw VideoError.message("the alpha encoder's first sample has no format description")
        }
        return CompressedClip(
            samples: samples, decodeOrder: samples, format: description,
            usingHardware: Compressor.readFlag(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder),
            encoderID: Compressor.readString(session, kVTCompressionPropertyKey_EncoderID),
            seconds: CFAbsoluteTimeGetCurrent() - start,
            bytes: samples.reduce(0) { $0 + CMSampleBufferGetTotalSampleSize($1) })
    }

    public struct Decoded {
        public var frames = 0
        /// Frames whose colour planes carry the right barcode and pattern and whose alpha plane carries its ramp.
        public var matched = 0
        public var firstMismatch: String?
        public var errors: [OSStatus] = []
        /// From a frame's VTDecompressionSessionDecodeFrame call to its output.
        public var worstLatency = 0.0
        /// Frames out after a later one: decode order, which clients put right by presentation time.
        public var outOfDisplayOrder = 0
        /// DecodeFrame calls that returned before their frame came out (synchronous decoding wants none).
        public var returnedBeforeOutput = 0
        public var outputFormat: OSType = 0
        public var worstLuma = 0.0, worstChroma = 0.0, worstAlpha = 0.0
        public var usingHardware = false
        /// The Switch decoder's SwitchPictureRoutes before the first frame and after the last (its colour layer's).
        public var routesBefore: [String: Int]?
        public var routesAfter: [String: Int]?
    }

    /// AlphaClip through a VTDecompressionSession into 'v0a8': VideoToolbox's MuxedAlphaDecoder decodes it, one decoder
    /// session per layer. Playback asks for asynchronous decoding with temporal processing.
    public static func decode(
        _ samples: [CMSampleBuffer], format description: CMVideoFormatDescription, flags: VTDecodeFrameFlags
    ) throws -> Decoded {
        let destination = PatternPixels.attributes(
            width: AlphaClip.width, height: AlphaClip.height, format: Self.format)
        var made: VTDecompressionSession?
        let created = VTDecompressionSessionCreate(
            allocator: nil, formatDescription: description, decoderSpecification: nil,
            imageBufferAttributes: destination as CFDictionary, outputCallback: nil, decompressionSessionOut: &made)
        guard created == noErr, let session = made else {
            throw VideoError.status("VTDecompressionSessionCreate", created)
        }
        defer { VTDecompressionSessionInvalidate(session) }
        let outputs = Outputs()
        let before = Decompressor.pictureRoutes(session)
        for (index, sample) in samples.enumerated() {
            outputs.submitted(AlphaVideo.frame(sample.presentationTimeStamp))
            let status = VTDecompressionSessionDecodeFrame(
                session, sampleBuffer: sample, flags: flags, infoFlagsOut: nil
            ) { status, _, image, pts, _ in outputs.add(status, image, AlphaVideo.frame(pts)) }
            guard status == noErr else { throw VideoError.status("VTDecompressionSessionDecodeFrame \(index)", status) }
            outputs.returned(AlphaVideo.frame(sample.presentationTimeStamp))
        }
        let waited = VTDecompressionSessionWaitForAsynchronousFrames(session)
        guard waited == noErr else {
            throw VideoError.status("VTDecompressionSessionWaitForAsynchronousFrames", waited)
        }
        var result = outputs.result
        result.usingHardware = Compressor.readFlag(
            session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder)
        result.routesBefore = before
        result.routesAfter = Decompressor.pictureRoutes(session)
        return result
    }

    static func frame(_ time: CMTime) -> Int { Int((time.seconds * Double(AlphaClip.fps)).rounded()) }

    final class Outputs: @unchecked Sendable {
        private let lock = NSLock()
        private var submittedAt: [Int: Double] = [:]
        private var out: Set<Int> = []
        private var latest = -1
        private(set) var result = Decoded()

        func submitted(_ frame: Int) {
            lock.lock()
            submittedAt[frame] = CFAbsoluteTimeGetCurrent()
            lock.unlock()
        }

        func returned(_ frame: Int) {
            lock.lock()
            if !out.contains(frame) { result.returnedBeforeOutput += 1 }
            lock.unlock()
        }

        func add(_ status: OSStatus, _ image: CVImageBuffer?, _ frame: Int) {
            let now = CFAbsoluteTimeGetCurrent()
            var verdict: FrameCheck?
            var alpha: Double?
            if status == noErr, let image {
                verdict = PatternPixels.check(image, pattern: AlphaClip.pattern, expected: frame)
                alpha = AlphaVideo.alphaError(image, frame: frame)
            }
            lock.lock()
            defer { lock.unlock() }
            result.frames += 1
            out.insert(frame)
            if status != noErr { result.errors.append(status) }
            if let image { result.outputFormat = CVPixelBufferGetPixelFormatType(image) }
            if let at = submittedAt[frame] { result.worstLatency = max(result.worstLatency, now - at) }
            if frame < latest { result.outOfDisplayOrder += 1 }
            latest = max(latest, frame)
            if let verdict {
                result.worstLuma = max(result.worstLuma, verdict.lumaError)
                result.worstChroma = max(result.worstChroma, verdict.chromaError)
            }
            if let alpha { result.worstAlpha = max(result.worstAlpha, alpha) }
            if let verdict, let alpha, verdict.matches(frame: frame, tolerance: Decompressor.tolerance),
                alpha <= Decompressor.tolerance
            {
                result.matched += 1
            } else if result.firstMismatch == nil {
                result.firstMismatch =
                    verdict.map {
                        "frame \(frame): read \($0.frame.map(String.init) ?? "bad CRC"), luma error "
                            + String(format: "%.1f chroma error %.1f alpha error ", $0.lumaError, $0.chromaError)
                            + (alpha.map { String(format: "%.1f", $0) } ?? "no alpha plane")
                    } ?? "frame \(frame): decoder status \(status)"
            }
        }
    }

    /// The encoder's samples as it hands them over: decode order, which the file keeps.
    final class DecodeOrder: @unchecked Sendable {
        private let lock = NSLock()
        private var received: [CMSampleBuffer] = []
        private var failure: OSStatus = noErr

        func add(_ status: OSStatus, _ sample: CMSampleBuffer?) {
            lock.lock()
            defer { lock.unlock() }
            if status != noErr {
                failure = status
            } else if let sample {
                received.append(sample)
            }
        }

        func samples(expected: Int) throws -> [CMSampleBuffer] {
            lock.lock()
            defer { lock.unlock() }
            guard failure == noErr else { throw VideoError.status("alpha encoder output", failure) }
            guard received.count == expected else {
                throw VideoError.message("the alpha encoder returned \(received.count) of \(expected) frames")
            }
            return received
        }
    }

    public static func fill(_ buffer: CVPixelBuffer, frame: Int) {
        PatternPixels.fill(buffer, pattern: AlphaClip.pattern, frame: frame)
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        let base = CVPixelBufferGetBaseAddressOfPlane(buffer, 2)!
        let rowBytes = CVPixelBufferGetBytesPerRowOfPlane(buffer, 2)
        for y in 0..<AlphaClip.height {
            memset(base + y * rowBytes, Int32(AlphaClip.alpha(y: y, frame: frame)), AlphaClip.width)
        }
    }

    /// The worst mean error, in 8-bit steps, of 8x8 blocks of the alpha plane against frame `frame`'s ramp; nil for a
    /// buffer without an alpha plane.
    public static func alphaError(_ buffer: CVPixelBuffer, frame: Int) -> Double? {
        guard CVPixelBufferGetPixelFormatType(buffer) == format, CVPixelBufferGetPlaneCount(buffer) == 3 else {
            return nil
        }
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        let base = UnsafeRawPointer(CVPixelBufferGetBaseAddressOfPlane(buffer, 2)!)
        let rowBytes = CVPixelBufferGetBytesPerRowOfPlane(buffer, 2)
        var worst = 0.0
        for row in 0..<16 {
            let top = (row * 2 + 1) * AlphaClip.height / 32 - 4
            for column in 0..<8 {
                let left = (column * 2 + 1) * AlphaClip.width / 16 - 4
                var read = 0.0
                var expected = 0.0
                for y in top..<(top + 8) {
                    expected += Double(AlphaClip.alpha(y: y, frame: frame)) * 8
                    for x in left..<(left + 8) {
                        read += Double(base.load(fromByteOffset: y * rowBytes + x, as: UInt8.self))
                    }
                }
                worst = max(worst, abs(read - expected) / 64)
            }
        }
        return worst
    }
}
