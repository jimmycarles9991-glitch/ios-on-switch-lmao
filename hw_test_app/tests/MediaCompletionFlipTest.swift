import AVFoundation
import HWTestCore
import HWTestVideo
import UIKit

/// A video frame on windows B and C is written by its producers' own completion (host1x's syncpoint interrupt for the
/// VIC or NVDEC job that wrote it), with no hop to the display's work loop, which takes the set into its latch
/// afterwards (drivers/NativeMobileDisplay/SwitchMediaCompletionFlipDisplay.h). Plays the 1080p H.264 clip in a
/// visible AVPlayerLayer, as playback.h264_1080p does, and reads the kernel's counters around it
/// (kern.switch_media_flips_*, debug kernels): completions must have written the sets armed for them, each ahead of
/// the work loop, and none may have failed to latch as written (refused is the boot's total; a refusing unit turns
/// these flips off at the first one, with one SwitchDisplay line). A set whose producers finished before its swap goes
/// out at once from the kick and is never armed; with none armed the test is skipped, armed=0 says why. filter_share
/// is the part host1x's syncpoint filter wrote earlier still.
struct MediaCompletionFlipTest: HWTest {
    let id = "display.media_completion_flip"
    let name = "Video sets on windows B and C written by their producers' completions"
    let subsystem = Subsystem.display

    static let counters = ["completion", "withdrawn", "refused", "lead_us", "armed", "filter", "filter_declined"]

    func run(_ context: TestContext) async throws -> Outcome {
        let (before, unread) = Self.totals()
        guard !before.isEmpty else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(unread))
            return .skip(KernelSysctl.unreadable("kern.switch_media_flips_*", unread), metrics)
        }
        guard let clip = Clip.all.first(where: { $0.codec == .h264 && $0.height == 1080 }) else {
            return .skip("no 1080p H.264 clip", Metrics())
        }
        let item = await MainActor.run {
            AVPlayerItem(asset: AVURLAsset(url: context.media.appendingPathComponent(clip.file)))
        }
        let player = await MainActor.run { AVPlayer(playerItem: item) }
        await context.onStage { stage in
            let view = PlayerView(frame: stage.bounds)
            view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            view.playerLayer.player = player
            view.playerLayer.videoGravity = .resizeAspect
            stage.addSubview(view)
        }
        let ended = Waiter<Bool>()
        let observer = NotificationCenter.default.addObserver(
            forName: .AVPlayerItemDidPlayToEndTime, object: item, queue: .main
        ) { _ in ended.resume(true) }
        defer { NotificationCenter.default.removeObserver(observer) }
        await MainActor.run { player.play() }
        let finished = await ended.wait(timeout: Double(clip.frames) / Double(clip.fps) + 15)
        await MainActor.run { player.pause() }
        let after = Self.totals().values
        let delta = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }
        let dropped = (item.accessLog()?.events ?? []).reduce(0) { $0 + max(0, $1.numberOfDroppedVideoFrames) }
        let completion = delta("completion")
        let armed = delta("armed")
        let refused = after["refused"] ?? 0

        var metrics = Metrics()
        for name in Self.counters { metrics.set("d_" + name, delta(name)) }
        metrics.set("refused", refused)
        metrics.set("expected", clip.frames)
        metrics.set("completion_share", Double(completion) / Double(max(1, armed)), digits: 3)
        metrics.set("lead_mean_us", completion > 0 ? Double(delta("lead_us")) / Double(completion) : 0, digits: 1)
        metrics.set("filter_share", completion > 0 ? Double(delta("filter")) / Double(completion) : 0, digits: 3)
        metrics.set("dropped", dropped)
        if armed == 0 && refused == 0 {
            return .skip("no video set waited for its producers at its swap", metrics)
        }
        return .judged(
            [
                ("windows B and C latched every completion's set as written (refused = 0)", refused == 0),
                ("played to the end", finished == true),
                ("completions wrote video sets (d_completion > 0)", completion > 0),
                (
                    "every armed set was written by its completion or withdrawn (one may straddle the end)",
                    completion + delta("withdrawn") + 1 >= armed
                ),
                ("ahead of the display's work loop (d_lead_us > 0)", delta("lead_us") > 0),
                ("no frames dropped", dropped == 0),
            ], metrics)
    }

    /// kern.switch_media_flips_* by name, and the errno of the first that could not be read.
    static func totals() -> (values: [String: Int], unread: Int32) {
        var values: [String: Int] = [:]
        var unread: Int32 = 0
        for name in counters {
            switch KernelSysctl.integer("kern.switch_media_flips_" + name, as: Int32.self) {
            case .value(let value): values[name] = Int(value)
            case .failed(let error): if unread == 0 { unread = error }
            }
        }
        return (values, unread)
    }
}
