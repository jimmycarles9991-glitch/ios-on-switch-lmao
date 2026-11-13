import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// The HEVC-with-alpha clip (AlphaClip: hierarchical B pictures, like the AirPods cards' movies) into 'v0a8' through
/// VideoToolbox's MuxedAlphaDecoder, which runs one hardware decoder session per layer. It allows four frames in flight
/// and fails the next one after 10 s (-12911); a synchronous DecodeFrame waits for both layers with no limit. So:
/// played as playback asks (asynchronous, temporal processing), every frame out within 100 ms of its call with no
/// error; then synchronously, still with temporal processing (the flag a layer decoder may hold pictures under), every
/// frame out before its DecodeFrame returns. Every frame's colour planes must carry the pattern and its alpha plane
/// the frame's ramp. On iOS 27 both layers go straight into VideoToolbox's target (AVDAlpha.c): the colour session's
/// SwitchPictureRoutes must show it writing from plane 0 by VIC, and the process's alpha-plane pictures by VIC must
/// rise by the frame count with none by the CPU. iOS 17's VideoToolbox merges the layers on the CPU.
struct HEVCAlphaTest: HWTest {
    let id = "decode.hevc_alpha"
    let name = "HEVC with alpha 1080x1200 into 'v0a8' (VideoToolbox's two-layer decoder)"
    let subsystem = Subsystem.decode
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let demuxed = try await Demux.mp4(context.media.appendingPathComponent(AlphaClip.file))
        let played = try AlphaVideo.decode(
            demuxed.samples, format: demuxed.format,
            flags: [._EnableAsynchronousDecompression, ._EnableTemporalProcessing])
        // Read-only from here on: the synchronous pass decodes them on its own thread.
        nonisolated(unsafe) let samples = demuxed.samples
        let format = demuxed.format
        let synchronous = Waiter<Result<AlphaVideo.Decoded, Error>>()
        Thread.detachNewThread {
            synchronous.resume(
                Result { try AlphaVideo.decode(samples, format: format, flags: [._EnableTemporalProcessing]) })
        }
        let waited = await synchronous.wait(timeout: 15)
        let sync = try waited?.get()

        var metrics = Metrics()
        metrics.set("frames", played.frames)
        metrics.set("matched", played.matched)
        metrics.set("worst_ms", played.worstLatency * 1000, digits: 1)
        metrics.set("out_of_display_order", played.outOfDisplayOrder)
        metrics.set("luma_err", played.worstLuma, digits: 1)
        metrics.set("chroma_err", played.worstChroma, digits: 1)
        metrics.set("alpha_err", played.worstAlpha, digits: 1)
        if !played.errors.isEmpty { metrics.set("errors", played.errors.map(String.init).joined(separator: ",")) }
        metrics.set("sync_frames", sync?.frames ?? -1)
        metrics.set("sync_late", sync?.returnedBeforeOutput ?? -1)
        var checks: [(String, Bool)] = [
            ("UsingHardwareAcceleratedVideoDecoder", played.usingHardware),
            ("'v0a8' out", played.outputFormat == AlphaVideo.format),
            ("no MuxedAlphaDecoder timeout (-12911) or other error", played.errors.isEmpty),
            ("every frame out", played.frames == AlphaClip.frames),
            ("every frame within 100 ms of its decode call", played.worstLatency < 0.1),
            (
                "colour and alpha decode to the pattern (\(played.firstMismatch ?? "ok"))",
                played.matched == AlphaClip.frames
            ),
            ("a synchronous decode returns within 15 s", sync != nil),
            (
                "synchronously, every frame out before its DecodeFrame returns",
                sync.map { $0.frames == AlphaClip.frames && $0.returnedBeforeOutput == 0 && $0.errors.isEmpty } ?? false
            ),
        ]
        let before = played.routesBefore ?? [:]
        let after = played.routesAfter ?? [:]
        let colour = { (key: String) in after[key] ?? -1 }
        let alpha = { (key: String) in (after[key] ?? 0) - (before[key] ?? 0) }
        for key in ["first_plane", "into_target", "vic", "cpu", "exported"] {
            metrics.set("decoder_" + key, colour(key))
        }
        metrics.set("alpha_vic", alpha("process_alpha_vic"))
        metrics.set("alpha_cpu", alpha("process_alpha_cpu"))
        if context.ios >= 27 {
            checks += [
                ("the decoder answers SwitchPictureRoutes (a debug AVD)", played.routesAfter != nil),
                (
                    "the colour layer goes into VideoToolbox's target from plane 0, by VIC",
                    colour("first_plane") == 0 && colour("into_target") == AlphaClip.frames
                        && colour("vic") == AlphaClip.frames && colour("cpu") == 0
                ),
                (
                    "the alpha layer goes into plane 2 by VIC, none by the CPU",
                    alpha("process_alpha_vic") == AlphaClip.frames && alpha("process_alpha_cpu") == 0
                ),
            ]
        }
        return .judged(checks, metrics, proof: played.usingHardware ? "UsingHardwareAcceleratedVideoDecoder=1" : "")
    }
}
