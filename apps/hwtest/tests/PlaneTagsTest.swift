import AVFoundation
import Darwin
import HWTestCore
import HWTestKit
import HWTestShared
import HWTestVideo
import UIKit

/// Detached video on the display's window B with the colour tags IOSurface's bulk attachments carry: the H.264 1080p
/// clip (BT.709 matrix, Left or no siting: window B's own CSC) and the MPEG-1 720p clip (the decoder tags Center
/// siting: TegraVIC converts each new frame to BGRA), each played to its end in an AVPlayerLayer full screen, with the
/// kernel's plane counters (kern.switch_planes_*, debug kernels) read around it. A detached layer that goes unshown
/// (plane_tags_<clip>_dropped above 0) is a tag the display or TegraVIC refuses; window B must flip for the 709 clip
/// (plane_tags_h264_flips). Which CSC window B programs is not readable from the app.
struct PlaneTagsTest: HWTest {
    let id = "display.plane_tags"
    let name = "Tagged 709 and Center-sited video detach onto window B with no plane dropped"
    let subsystem = Subsystem.compositor
    var timeout: TimeInterval { 90 }

    static let clips: [(key: String, codec: VideoCodec, height: Int)] = [("h264", .h264, 1080), ("mpeg1", .mpeg1, 720)]

    func run(_ context: TestContext) async throws -> Outcome {
        let (first, unread) = PlanesTest.totals()
        guard !first.isEmpty else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(unread))
            return .skip(
                KernelSysctl.unreadable("kern.switch_planes_*", unread, flag: "--//bazel:stats_frame"), metrics
            )
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for arm in Self.clips {
            guard let clip = Clip.all.first(where: { $0.codec == arm.codec && $0.height == arm.height }) else {
                checks.append(("no \(arm.key) clip", false))
                continue
            }
            let before = PlanesTest.totals().values
            let finished = try await Self.play(context, clip)
            let after = PlanesTest.totals().values
            let delta = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }
            for name in ["flips", "latched", "dropped"] { metrics.set("plane_tags_\(arm.key)_\(name)", delta(name)) }
            checks.append(("\(arm.key) played to the end", finished))
            let dropped = delta("dropped"), flips = delta("flips")
            checks.append(("\(arm.key): no detached layer went unshown (dropped \(dropped))", dropped == 0))
            if arm.codec == .h264 { checks.append(("h264: window B flipped (flips \(flips))", flips > 0)) }
        }
        return .judged(checks, metrics)
    }

    static func play(_ context: TestContext, _ clip: Clip) async throws -> Bool {
        let asset = AVURLAsset(url: context.media.appendingPathComponent(clip.file))
        let duration = try await asset.load(.duration)
        let source = asset
        let (item, player) = await MainActor.run { () -> (AVPlayerItem, AVPlayer) in
            let item = AVPlayerItem(asset: source)
            return (item, AVPlayer(playerItem: item))
        }
        await context.onStage { stage in
            let view = PlayerView(frame: stage.bounds)
            view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            view.playerLayer.player = player
            view.playerLayer.videoGravity = .resizeAspect
            stage.addSubview(view)
        }
        let ended = Waiter<Bool>()
        let observer = NotificationCenter.default.addObserver(
            forName: AVPlayerItem.didPlayToEndTimeNotification, object: item, queue: .main
        ) { _ in ended.resume(true) }
        defer { NotificationCenter.default.removeObserver(observer) }
        await MainActor.run { player.play() }
        let finished = await ended.wait(timeout: duration.seconds + 15)
        await MainActor.run { player.pause() }
        await context.clearStage()
        return finished == true
    }
}
