import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// 1080p60 pattern frames through a hardware-only VTCompressionSession (P pictures, no reordering), then decoded
/// again on the hardware decoder and checked frame by frame.
struct VideoEncodeTest: HWTest {
    let codec: VideoCodec
    let realTime: Bool
    let subsystem = Subsystem.encode
    static let frames = 60
    var id: String { "encode.\(codec.rawValue)_\(realTime ? "realtime" : "quality")" }
    var name: String { "\(VideoDecodeTest.codecName(codec)) encode, \(realTime ? "real-time" : "quality") preset" }
    var timeout: TimeInterval { 90 }

    static func hardwareEncoders() -> [[String: Any]] {
        var list: CFArray?
        guard VTCopyVideoEncoderList(nil, &list) == noErr, let encoders = list as? [[String: Any]] else { return [] }
        return encoders.filter { ($0[kVTVideoEncoderList_IsHardwareAccelerated as String] as? Bool) == true }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let type = codec == .h264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC
        let offered = Self.hardwareEncoders().filter {
            ($0[kVTVideoEncoderList_CodecType as String] as? UInt32) == type
        }
        if offered.isEmpty {
            if codec == .h264 { return .fail("VTCopyVideoEncoderList offers no hardware H.264 encoder") }
            return .skip("VTCopyVideoEncoderList offers no hardware HEVC encoder")
        }
        let (width, height, fps) = (1920, 1080, 60)
        let pattern = Pattern(width: width, height: height, clip: realTime ? 0x7001 : 0x7002)
        let settings = CompressorSettings(
            codec: codec, width: width, height: height, fps: fps, bitrate: realTime ? 8_000_000 : 12_000_000,
            realTime: realTime, requireHardware: true)
        let encoded = try Compressor.encode(settings, pattern: pattern, frames: Self.frames)
        let decoded = try Decompressor.decode(
            encoded.samples, format: encoded.format, pattern: pattern, bitDepth: 8, fps: fps)
        let seconds = Double(Self.frames) / Double(fps)
        var metrics = Metrics()
        metrics.set("fps", Double(Self.frames) / encoded.seconds, digits: 1)
        metrics.set("mbps", Double(encoded.bytes * 8) / seconds / 1e6, digits: 2)
        metrics.set("key_frames", encoded.keyFrames)
        metrics.set("p_frames", encoded.predictedFrames)
        metrics.set("decoded_match", decoded.matched)
        metrics.set("luma_err", decoded.worstLumaError, digits: 1)
        return .judged(
            [
                ("UsingHardwareAcceleratedVideoEncoder", encoded.usingHardware),
                ("P pictures after the first key frame", encoded.predictedFrames >= Self.frames - 2),
                ("decodes back to the pattern (\(decoded.firstMismatch ?? "ok"))", decoded.matched == Self.frames),
                ("real time keeps up with 60 fps", !realTime || Double(Self.frames) / encoded.seconds >= Double(fps)),
            ], metrics, proof: encoded.usingHardware ? "encoder=\(encoded.encoderID)" : "")
    }

    static var all: [VideoEncodeTest] {
        [VideoCodec.h264, .hevc].flatMap { codec in [true, false].map { VideoEncodeTest(codec: codec, realTime: $0) } }
    }
}
