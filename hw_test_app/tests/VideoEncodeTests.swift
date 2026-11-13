import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// 1080p60 pattern frames through a hardware-only VTCompressionSession (P pictures, no reordering), then decoded
/// again on the hardware decoder and checked frame by frame. The frames come from the session's pool, and every
/// picture's input must reach NVENC without a CPU copy: VIC writes it from the frame's IOSurface (SwitchEncoderRoutes).
/// encode.h264_client_pool hands over frames from a pool of CoreVideo's defaults instead (1920-byte rows at 1080p, off
/// VIC's 256-byte grid), as a client with buffers of its own does, and is judged the same: VideoToolbox moves a frame
/// session's pictures onto the grid the encoder asked for (it did on the Mac, test/ave_host.c), and rows that still
/// arrive off it are bounced through the copy engine in the kernel (input_bounced), so no input is copied or refused.
struct VideoEncodeTest: HWTest {
    let codec: VideoCodec
    let realTime: Bool
    var clientPool = false
    let subsystem = Subsystem.encode
    static let frames = 60
    var id: String {
        clientPool
            ? "encode.\(codec.rawValue)_client_pool" : "encode.\(codec.rawValue)_\(realTime ? "realtime" : "quality")"
    }
    var name: String {
        let input = clientPool ? "the client's own buffers" : realTime ? "real-time preset" : "quality preset"
        return "\(VideoDecodeTest.codecName(codec)) encode, \(input)"
    }
    var timeout: TimeInterval { 90 }

    static func hardwareEncoders() -> [[String: Any]] {
        var list: CFArray?
        guard VTCopyVideoEncoderList(nil, &list) == noErr, let encoders = list as? [[String: Any]] else { return [] }
        return encoders.filter { ($0[kVTVideoEncoderList_IsHardwareAccelerated as String] as? Bool) == true }
    }

    // The session is always created, listed or not: what VTCompressionSessionCreate answers is the evidence.
    func run(_ context: TestContext) async throws -> Outcome {
        let type = codec == .h264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC
        let offered = Self.hardwareEncoders().filter {
            ($0[kVTVideoEncoderList_CodecType as String] as? UInt32) == type
        }
        let listed = offered.compactMap { $0[kVTVideoEncoderList_EncoderID as String] as? String }
        let (width, height, fps) = (1920, 1080, 60)
        let pattern = Pattern(width: width, height: height, clip: realTime ? 0x7001 : 0x7002)
        let settings = CompressorSettings(
            codec: codec, width: width, height: height, fps: fps, bitrate: realTime ? 8_000_000 : 12_000_000,
            realTime: realTime, requireHardware: true)
        let encoded: CompressedClip
        do {
            encoded = try Compressor.encode(settings, pattern: pattern, frames: Self.frames, clientPool: clientPool)
        } catch VideoError.status(let call, let status) where codec == .hevc && listed.isEmpty {
            return .skip("no hardware HEVC encoder: none listed, and \(call) returned \(status)")
        }
        let decoded = try Decompressor.decode(
            encoded.samples, format: encoded.format, pattern: pattern, bitDepth: 8, fps: fps)
        let seconds = Double(Self.frames) / Double(fps)
        var metrics = Metrics()
        metrics.set("listed", listed.isEmpty ? "-" : listed.joined(separator: ","))
        metrics.set("fps", Double(Self.frames) / encoded.seconds, digits: 1)
        metrics.set("mbps", Double(encoded.bytes * 8) / seconds / 1e6, digits: 2)
        metrics.set("key_frames", encoded.keyFrames)
        metrics.set("p_frames", encoded.predictedFrames)
        metrics.set("decoded_match", decoded.matched)
        metrics.set("luma_err", decoded.worstLumaError, digits: 1)
        metrics.set("intra_only", encoded.predictedFrames == 0)
        metrics.set("input_vic", encoded.routes?["input_vic"] ?? -1)
        metrics.set("input_cpu_copies", encoded.routes?["input_cpu_copies"] ?? -1)
        metrics.set("input_refused", encoded.routes?["input_refused"] ?? -1)
        metrics.set("input_bounced", encoded.routes?["input_bounced"] ?? -1)
        return .judged(
            [
                ("VTCopyVideoEncoderList offers it as hardware", !listed.isEmpty),
                ("UsingHardwareAcceleratedVideoEncoder", encoded.usingHardware),
                ("P pictures after the first key frame", encoded.predictedFrames >= Self.frames - 2),
                ("decodes back to the pattern (\(decoded.firstMismatch ?? "ok"))", decoded.matched == Self.frames),
                ("real time keeps up with 60 fps", !realTime || Double(Self.frames) / encoded.seconds >= Double(fps)),
                (
                    "every input written by VIC, none copied or refused",
                    Self.allByVIC(encoded.routes, Self.frames)
                ),
            ], metrics, proof: encoded.usingHardware ? "encoder=\(encoded.encoderID)" : "")
    }

    /// The AVE plug-in's input routes (debug builds): VIC wrote every picture's input, the CPU copied none.
    /// The AVE plug-in's input routes (debug builds): VIC wrote every picture's input, the CPU copied none and none
    /// was refused.
    static func allByVIC(_ routes: [String: Int]?, _ pictures: Int) -> Bool {
        guard let routes, let vic = routes["input_vic"] else { return false }
        return routes["input_cpu_copies"] == 0 && routes["input_refused"] == 0 && vic >= pictures
    }

    static var all: [VideoEncodeTest] {
        [VideoCodec.h264, .hevc].flatMap { codec in [true, false].map { VideoEncodeTest(codec: codec, realTime: $0) } }
            + [VideoEncodeTest(codec: .h264, realTime: false, clientPool: true)]
    }
}
