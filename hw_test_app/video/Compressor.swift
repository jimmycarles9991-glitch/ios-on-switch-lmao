import CoreMedia
import CoreVideo
import Foundation
import HWTestCore
import VideoToolbox

/// The pattern encoded by VideoToolbox, P pictures only (no frame reordering): decode order is display order.
public struct CompressedClip {
    public let samples: [CMSampleBuffer]
    public let format: CMVideoFormatDescription
    public let usingHardware: Bool
    public let encoderID: String
    public let seconds: Double
    public let bytes: Int

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
    public static func encode(_ settings: CompressorSettings, pattern: Pattern, frames: Int) throws -> CompressedClip {
        let session = try makeSession(settings)
        defer { VTCompressionSessionInvalidate(session) }
        let pool = try PatternPixels.pool(
            width: settings.width, height: settings.height,
            format: PatternPixels.format(bitDepth: settings.codec.bitDepth))
        let inputs = try (0..<frames).map {
            try PatternPixels.make(pattern, frame: $0, bitDepth: settings.codec.bitDepth, pool: pool)
        }
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
        }
        let flushed = VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
        let seconds = CFAbsoluteTimeGetCurrent() - start
        guard flushed == noErr else { throw VideoError.status("VTCompressionSessionCompleteFrames", flushed) }
        let samples = try collector.result(expected: frames)
        guard let format = CMSampleBufferGetFormatDescription(samples[0]) else {
            throw VideoError.message("the encoder's first sample has no format description")
        }
        return CompressedClip(
            samples: samples, format: format,
            usingHardware: readFlag(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder),
            encoderID: readString(session, kVTCompressionPropertyKey_EncoderID), seconds: seconds,
            bytes: samples.reduce(0) { $0 + CMSampleBufferGetTotalSampleSize($1) })
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
            kVTCompressionPropertyKey_AllowFrameReordering: false,
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

    static func read(_ session: VTSession, _ key: CFString) -> CFTypeRef? {
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
            return samples.sorted { $0.presentationTimeStamp < $1.presentationTimeStamp }
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
