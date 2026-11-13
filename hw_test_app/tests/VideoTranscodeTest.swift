import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// A transcode with no copy of any picture: the 8-bit H.264 720p clip decoded on NVDEC, whose pictures the AVD plug-in
/// exports as the destination surfaces' native NVDEC pictures (decode.native_export counts the same), handed untouched to
/// a hardware H.264 session; NVENC reads each native picture in place (TegraNVENCDirect.cpp), with no VIC job and no CPU
/// copy (SwitchEncoderRoutes input_direct), and the stream it codes decodes back to the clip's pattern frame by frame.
struct VideoTranscodeTest: HWTest {
    let id = "encode.h264_from_nvdec"
    let name = "H.264 720p decoded on NVDEC and its native pictures encoded on NVENC in place"
    let subsystem = Subsystem.encode
    var timeout: TimeInterval { 90 }

    func run(_ context: TestContext) async throws -> Outcome {
        guard let clip = Clip.all.first(where: { $0.codec == .h264 && $0.height == 720 }) else {
            return .skip("no H.264 720p clip")
        }
        let demuxed = try await Demux.load(clip, from: context.media)
        let decoded = try Decompressor.decodePictures(
            demuxed.samples, format: demuxed.format, width: clip.width, height: clip.height, bitDepth: 8)
        let frames = decoded.pictures.count
        let settings = CompressorSettings(
            codec: .h264, width: clip.width, height: clip.height, fps: clip.fps, bitrate: 8_000_000, realTime: false,
            requireHardware: true)
        let encoded = try Compressor.encode(settings, pictures: decoded.pictures)
        let back = try Decompressor.decode(
            encoded.samples, format: encoded.format, pattern: clip.pattern, bitDepth: 8, fps: clip.fps)
        let routes = encoded.routes
        var metrics = Metrics()
        metrics.set("frames", frames)
        for key in ["exported", "vic", "cpu"] { metrics.set("decoder_" + key, decoded.routes?[key] ?? -1) }
        for key in ["input_direct", "input_vic", "input_bounced", "input_cpu_copies", "input_refused"] {
            metrics.set(key, routes?[key] ?? -1)
        }
        metrics.set("decoded_match", back.matched)
        metrics.set("luma_err", back.worstLumaError, digits: 1)
        return .judged(
            [
                ("the decoder exported every picture natively", decoded.routes?["exported"] == frames),
                ("UsingHardwareAcceleratedVideoEncoder", encoded.usingHardware),
                (
                    "NVENC read every native picture in place: no VIC job, no CPU copy",
                    routes?["input_direct"] == frames && routes?["input_vic"] == 0
                        && routes?["input_cpu_copies"] == 0 && routes?["input_refused"] == 0
                ),
                ("decodes back to the pattern (\(back.firstMismatch ?? "ok"))", back.matched == frames),
            ], metrics, proof: "input_direct=\(routes?["input_direct"].map(String.init) ?? "-") of \(frames)")
    }
}
