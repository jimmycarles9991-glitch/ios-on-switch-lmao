import CoreMedia
import CoreVideo
import Foundation
import HWTestCore
import VideoToolbox

/// The pattern encoded by VideoToolbox. `decodeOrder` is the order the encoder put the samples out in; Compressor's
/// `samples` are in display order (the same unless the session allows frame reordering).
public struct CompressedClip {
    public let samples: [CMSampleBuffer]
    public let decodeOrder: [CMSampleBuffer]
    public let format: CMVideoFormatDescription
    public let usingHardware: Bool
    public let encoderID: String
    public let seconds: Double
    public let bytes: Int
    /// SwitchEncoderRoutes (debug builds of the AVE plug-in): pictures, input_vic, input_bounced, input_cpu_copies,
    /// input_refused; nil from any other encoder.
    public var routes: [String: Int]? = nil

    public var keyFrames: Int { samples.filter(\.isKeyFrame).count }
    public var predictedFrames: Int { samples.count - keyFrames }
}

public struct CompressorSettings {
    public var codec: VideoCodec
    public var width: Int
    public var height: Int
    public var fps: Int
    public var bitrate: Int
    public var realTime: Bool
    /// kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: no software fallback.
    public var requireHardware: Bool
    /// kVTCompressionPropertyKey_AllowFrameReordering (B pictures).
    public var allowReordering = false

    public init(
        codec: VideoCodec, width: Int, height: Int, fps: Int, bitrate: Int, realTime: Bool, requireHardware: Bool
    ) {
        self.codec = codec
        self.width = width
        self.height = height
        self.fps = fps
        self.bitrate = bitrate
        self.realTime = realTime
        self.requireHardware = requireHardware
    }

    public init(_ clip: Clip, requireHardware: Bool) {
        self.init(
            codec: clip.codec, width: clip.width, height: clip.height, fps: clip.fps, bitrate: clip.bitrate,
            realTime: false, requireHardware: requireHardware)
    }

    var codecType: CMVideoCodecType { codec == .h264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC }

    var profile: CFString {
        switch codec {
        case .h264: return kVTProfileLevel_H264_High_AutoLevel
        case .hevc10: return kVTProfileLevel_HEVC_Main10_AutoLevel
        default: return kVTProfileLevel_HEVC_Main_AutoLevel
        }
    }
}

public enum Compressor {
    /// `pause`: the encoder sits idle for that many seconds after that frame, then the clip goes on. The pictures come
    /// from the session's pixel buffer pool, laid out as the encoder asked, as a client should take them, or with
    /// `clientPool` from a pool of CoreVideo's defaults, as a client with its own buffers hands them over. `inspect`
    /// sees the session after the last frame, before it goes.
    public static func encode(
        _ settings: CompressorSettings, pattern: Pattern, frames: Int, pause: (after: Int, seconds: Double)? = nil,
        clientPool: Bool = false, inspect: ((VTCompressionSession) throws -> Void)? = nil
    ) throws -> CompressedClip {
        let session = try makeSession(settings)
        defer { VTCompressionSessionInvalidate(session) }
        let sessionPool = clientPool ? nil : VTCompressionSessionGetPixelBufferPool(session)
        let pool =
            try sessionPool
            ?? PatternPixels.pool(
                width: settings.width, height: settings.height,
                format: PatternPixels.format(bitDepth: settings.codec.bitDepth))
        let inputs = try (0..<frames).map {
            try PatternPixels.make(pattern, frame: $0, bitDepth: settings.codec.bitDepth, pool: pool)
        }
        return try encode(session, settings, inputs: inputs, pause: pause, inspect: inspect)
    }

    /// `pictures` as they are (frame i at i / fps), the client's own buffers: a decoder's, say.
    public static func encode(
        _ settings: CompressorSettings, pictures: [CVPixelBuffer],
        inspect: ((VTCompressionSession) throws -> Void)? = nil
    ) throws -> CompressedClip {
        let session = try makeSession(settings)
        defer { VTCompressionSessionInvalidate(session) }
        return try encode(session, settings, inputs: pictures, pause: nil, inspect: inspect)
    }

    static func encode(
        _ session: VTCompressionSession, _ settings: CompressorSettings, inputs: [CVPixelBuffer],
        pause: (after: Int, seconds: Double)?, inspect: ((VTCompressionSession) throws -> Void)?
    ) throws -> CompressedClip {
        let frames = inputs.count
        let collector = Collector()
        let start = CFAbsoluteTimeGetCurrent()
        for (index, input) in inputs.enumerated() {
            let properties = index == 0 ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
            let status = VTCompressionSessionEncodeFrame(
                session, imageBuffer: input,
                presentationTimeStamp: CMTime(value: CMTimeValue(index), timescale: CMTimeScale(settings.fps)),
                duration: CMTime(value: 1, timescale: CMTimeScale(settings.fps)), frameProperties: properties,
                infoFlagsOut: nil
            ) { status, _, sample in collector.add(status, sample) }
            guard status == noErr else { throw VideoError.status("VTCompressionSessionEncodeFrame \(index)", status) }
            if let pause, pause.after == index {
                let flushed = VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
                guard flushed == noErr else { throw VideoError.status("VTCompressionSessionCompleteFrames", flushed) }
                Thread.sleep(forTimeInterval: pause.seconds)
            }
        }
        let flushed = VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
        let seconds = CFAbsoluteTimeGetCurrent() - start
        guard flushed == noErr else { throw VideoError.status("VTCompressionSessionCompleteFrames", flushed) }
        let decodeOrder = try collector.result(expected: frames)
        let samples = decodeOrder.sorted { $0.presentationTimeStamp < $1.presentationTimeStamp }
        guard let format = CMSampleBufferGetFormatDescription(samples[0]) else {
            throw VideoError.message("the encoder's first sample has no format description")
        }
        try inspect?(session)
        return CompressedClip(
            samples: samples, decodeOrder: decodeOrder, format: format,
            usingHardware: readFlag(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder),
            encoderID: readString(session, kVTCompressionPropertyKey_EncoderID), seconds: seconds,
            bytes: samples.reduce(0) { $0 + CMSampleBufferGetTotalSampleSize($1) },
            routes: (read(session, "SwitchEncoderRoutes" as CFString) as? [String: NSNumber])?.mapValues(\.intValue))
    }

    static func makeSession(_ settings: CompressorSettings) throws -> VTCompressionSession {
        var specification: [CFString: Any] = [:]
        if settings.requireHardware {
            specification[kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder] = true
        } else {
            specification[kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder] = true
        }
        var session: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: nil, width: Int32(settings.width), height: Int32(settings.height), codecType: settings.codecType,
            encoderSpecification: specification as CFDictionary, imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: nil, refcon: nil, compressionSessionOut: &session)
        guard status == noErr, let session else { throw VideoError.status("VTCompressionSessionCreate", status) }
        let properties: [CFString: Any] = [
            kVTCompressionPropertyKey_RealTime: settings.realTime,
            kVTCompressionPropertyKey_AllowFrameReordering: settings.allowReordering,
            kVTCompressionPropertyKey_ProfileLevel: settings.profile,
            kVTCompressionPropertyKey_AverageBitRate: settings.bitrate,
            kVTCompressionPropertyKey_ExpectedFrameRate: settings.fps,
            kVTCompressionPropertyKey_MaxKeyFrameInterval: 100_000,
            kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality: settings.realTime,
        ]
        for (key, value) in properties {
            let set = VTSessionSetProperty(session, key: key, value: value as CFTypeRef)
            // An encoder may lack an optional knob (speed priority); the bitrate, profile and ordering must hold.
            if set != noErr && key != kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality {
                VTCompressionSessionInvalidate(session)
                throw VideoError.status("VTSessionSetProperty \(key)", set)
            }
        }
        VTCompressionSessionPrepareToEncodeFrames(session)
        return session
    }

    public static func read(_ session: VTSession, _ key: CFString) -> CFTypeRef? {
        var value: Unmanaged<CFTypeRef>?
        guard VTSessionCopyProperty(session, key: key, allocator: nil, valueOut: &value) == noErr else { return nil }
        return value?.takeRetainedValue()
    }

    static func readFlag(_ session: VTSession, _ key: CFString) -> Bool { (read(session, key) as? Bool) ?? false }

    static func readString(_ session: VTSession, _ key: CFString) -> String { (read(session, key) as? String) ?? "" }

    final class Collector: @unchecked Sendable {
        private let lock = NSLock()
        private var samples: [CMSampleBuffer] = []
        private var failure: OSStatus = noErr

        func add(_ status: OSStatus, _ sample: CMSampleBuffer?) {
            lock.lock()
            defer { lock.unlock() }
            if status != noErr {
                failure = status
            } else if let sample {
                samples.append(sample)
            }
        }

        func result(expected: Int) throws -> [CMSampleBuffer] {
            lock.lock()
            defer { lock.unlock() }
            guard failure == noErr else { throw VideoError.status("encoder output", failure) }
            guard samples.count == expected else {
                throw VideoError.message("the encoder returned \(samples.count) of \(expected) frames")
            }
            return samples
        }
    }
}

extension CMSampleBuffer {
    public var isKeyFrame: Bool {
        guard
            let attachments = CMSampleBufferGetSampleAttachmentsArray(self, createIfNecessary: false)
                as? [[CFString: Any]],
            let first = attachments.first
        else { return true }
        return !((first[kCMSampleAttachmentKey_NotSync] as? Bool) ?? false)
    }
}
