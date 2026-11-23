import AVFoundation
import Foundation
import HWTestCore
import HWTestVideo
import UIKit

/// The landscape route at the size display.planes and playback.* show their video: the H.264 1080p clip in the test
/// stage, about 3.6x down both ways once turned (TegraVIC's '420v' 1920x1080 -> 300x534), where boot 22's window A
/// underflowed from the first landscape frame and the overlay recovery left and re-entered landscape every 100 ms.
/// Window A column-scans it with each initial DDA a fraction (SwitchLandscape.h) and every window's memfetch reset with
/// each change of mode (SwitchLandscapeDisplay.h). Pass: the video is landscape over the whole sample, no window
/// underflows, no change of mode fails, and the display server asks VIC for no job.
struct LandscapeDownscaleTest: HWTest {
    let id = "display.x1.landscape_downscale"
    let name = "A downscaled landscape video column-scanned on window A with no underflow"
    let subsystem = Subsystem.display
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        guard let clip = Clip.all.first(where: { $0.codec == .h264 && $0.height == 1080 }) else {
            return .skip("no H.264 1080p clip")
        }
        let session: VICKernelTest.Session
        switch VICKernelTest.Session.open() {
        case .failure(let failure): return .skip("no TegraVIC kernel-test connection: \(failure)")
        case .success(let opened): session = opened
        }
        defer { session.close() }
        let asset = AVURLAsset(url: context.media.appendingPathComponent(clip.file))
        let duration = try await asset.load(.duration)
        let composition = AVMutableComposition()
        for _ in 0 ..< 4 {
            try await composition.insertTimeRange(
                CMTimeRange(start: .zero, duration: duration), of: asset, at: composition.duration
            )
        }
        nonisolated(unsafe) let timeline = composition
        let player = await MainActor.run { AVPlayer(playerItem: AVPlayerItem(asset: timeline)) }
        let stage = await context.onStage { stage -> CGSize in
            let view = PlayerView(frame: stage.bounds)
            view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            view.playerLayer.player = player
            view.playerLayer.videoGravity = .resizeAspect
            stage.addSubview(view)
            return stage.bounds.size
        }
        let route = LandscapeVideoTest()
        guard case .success(let before) = await route.sample(session, frames: 2) else {
            return .fail("no sample before playing")
        }
        await MainActor.run { player.play() }
        try? await Task.sleep(nanoseconds: 1_000_000_000)
        let played = await route.sample(session, frames: 180)
        await MainActor.run { player.pause() }
        guard case .success(let playing) = played else { return .fail("no playing sample") }

        var metrics = Metrics()
        metrics.set("stage", "\(Int(stage.width))x\(Int(stage.height))")
        metrics.set("frames", playing.frames)
        metrics.set("landscape_frames", playing.landscapeFrames)
        metrics.set("underflows_a", playing.underflows[0])
        metrics.set("underflows_b", playing.underflows[1])
        metrics.set("underflows_c", playing.underflows[2])
        metrics.set("failures", playing.failures - before.failures)
        metrics.set("vic_jobs_display", playing.vicJobs[LandscapeSample.displayServer])
        return .judged(
            [
                ("the video stayed landscape over the sample (landscape_frames = frames)", playing.allLandscape),
                ("no window underflowed (underflows_a/b/c 0)", playing.underflowFrames == 0),
                ("no change of mode failed (failures 0)", playing.failures == before.failures),
                (
                    "the display server asked VIC for no job (vic_jobs_display 0)",
                    playing.vicJobs[LandscapeSample.displayServer] == 0
                ),
            ], metrics
        )
    }
}
