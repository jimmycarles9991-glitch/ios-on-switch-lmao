import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// A session that sits idle longer than the encoder's idle power-off (SwitchEngineGate, 500 ms) and goes on with P
/// pictures: the engine is powered off and on again between two pictures of one reference chain, and the clip must
/// still decode back exactly.
struct VideoEncodeIdleTest: HWTest {
    let codec: VideoCodec
    let subsystem = Subsystem.encode
    static let frames = 8
    static let pauseAfter = 2
    static let pauseSeconds = 1.2
    var id: String { "encode.\(codec.rawValue)_idle_gap" }
    var name: String { "\(VideoDecodeTest.codecName(codec)) encode across an idle power-off" }
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let (width, height, fps) = (1280, 720, 30)
        let pattern = Pattern(width: width, height: height, clip: 0x7003)
        let settings = CompressorSettings(
            codec: codec, width: width, height: height, fps: fps, bitrate: 4_000_000, realTime: false,
            requireHardware: true)
        let encoded: CompressedClip
        do {
            encoded = try Compressor.encode(
                settings, pattern: pattern, frames: Self.frames, pause: (Self.pauseAfter, Self.pauseSeconds))
        } catch VideoError.status(let call, let status)
            where codec == .hevc
            && VideoEncodeTest.hardwareEncoders()
                .allSatisfy({ ($0[kVTVideoEncoderList_CodecType as String] as? UInt32) != kCMVideoCodecType_HEVC })
        {
            return .skip("no hardware HEVC encoder: none listed, and \(call) returned \(status)")
        }
        let decoded = try Decompressor.decode(
            encoded.samples, format: encoded.format, pattern: pattern, bitDepth: 8, fps: fps)
        let afterPause = encoded.samples[(Self.pauseAfter + 1)...]
        var metrics = Metrics()
        metrics.set("pause_ms", Int(Self.pauseSeconds * 1000))
        metrics.set("key_frames", encoded.keyFrames)
        metrics.set("p_after_pause", afterPause.filter { !$0.isKeyFrame }.count)
        metrics.set("decoded_match", decoded.matched)
        metrics.set("luma_err", decoded.worstLumaError, digits: 1)
        metrics.set("input_vic", encoded.routes?["input_vic"] ?? -1)
        metrics.set("input_cpu_copies", encoded.routes?["input_cpu_copies"] ?? -1)
        metrics.set("input_refused", encoded.routes?["input_refused"] ?? -1)
        return .judged(
            [
                ("UsingHardwareAcceleratedVideoEncoder", encoded.usingHardware),
                ("one key frame, P pictures across the pause", encoded.keyFrames == 1),
                ("decodes back to the pattern (\(decoded.firstMismatch ?? "ok"))", decoded.matched == Self.frames),
                (
                    "every input written by VIC across the power-off, none copied or refused",
                    VideoEncodeTest.allByVIC(encoded.routes, Self.frames)
                ),
            ], metrics, proof: encoded.usingHardware ? "encoder=\(encoded.encoderID)" : "")
    }

    static var all: [VideoEncodeIdleTest] { [VideoCodec.h264, .hevc].map { VideoEncodeIdleTest(codec: $0) } }
}
