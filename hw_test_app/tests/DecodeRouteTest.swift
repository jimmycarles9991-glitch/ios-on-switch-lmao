import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// The H.264 720p clip decoded asynchronously, fed as fast as VideoToolbox takes it, and the AVD plugin's own count of
/// where its decode thread waited (SwitchPictureRoutes, debug builds): each picture goes out from its NVDEC job's
/// completion, so the thread calling DecodeFrame waits for none (picture_waits and sync_waits 0) while VIC or the
/// native export place every picture. in_flight_max and emit_latency_us say how far decoding ran ahead of emission;
/// room_waits counts the Submits that slept for room in NVDEC's queue. completion_port 1: the completions came on
/// TegraNVDEC's port (0: the decoding process's sandbox refused it and each job's Wait ran on the session's queue).
/// SKIP when the decoder does not say.
struct DecodeRouteTest: HWTest {
    let id = "decode.async_emit"
    let name = "H.264 720p decoded asynchronously: no decode-thread wait, by the decoder's count"
    let subsystem = Subsystem.decode
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        guard let clip = Clip.all.first(where: { $0.codec == .h264 && $0.height == 720 }) else {
            return .skip("no H.264 720p clip")
        }
        let demuxed = try await Demux.load(clip, from: context.media)
        let result = try Decompressor.decode(
            demuxed.samples, format: demuxed.format, pattern: clip.pattern, bitDepth: clip.codec.bitDepth,
            fps: clip.fps)
        var metrics = Metrics()
        metrics.set("frames", result.frames)
        metrics.set("matched", result.matched)
        guard let routes = result.routes else {
            return .skip("the decoder does not answer SwitchPictureRoutes (a release build)", metrics)
        }
        for key in Self.keys { metrics.set("decoder_" + key, routes[key] ?? -1) }
        let placed = (routes["cpu"] ?? -1) == 0 && routes["frames"] == clip.frames && routes["errors"] == 0
        let unwaited = routes["picture_waits"] == 0 && routes["sync_waits"] == 0
        let onPort = routes["completion_port"] == 1
        return .judged(
            [
                ("UsingHardwareAcceleratedVideoDecoder", result.usingHardware),
                ("every frame decodes to the pattern (\(result.firstMismatch ?? "ok"))", result.matched == clip.frames),
                ("every frame an NVDEC picture VIC or the export placed", placed),
                ("the decode thread waited for no picture (picture_waits 0, sync_waits 0)", unwaited),
                ("job completions came on TegraNVDEC's port (completion_port 1)", onPort),
            ], metrics,
            proof: "SwitchPictureRoutes picture_waits=\(routes["picture_waits"] ?? -1) "
                + "sync_waits=\(routes["sync_waits"] ?? -1) in_flight_max=\(routes["in_flight_max"] ?? -1) "
                + "completion_port=\(routes["completion_port"] ?? -1)")
    }

    static let keys = [
        "frames", "exported", "vic", "cpu", "errors", "picture_waits", "sync_waits", "dependency_waits", "room_waits",
        "in_flight_max", "emit_latency_us", "completion_port",
    ]
}
