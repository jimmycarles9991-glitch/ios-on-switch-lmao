import AVFoundation
import HWTestCore
import HWTestVideo
import QuartzCore
import UIKit

final class PlayerView: UIView {
    override class var layerClass: AnyClass { AVPlayerLayer.self }
    var playerLayer: AVPlayerLayer { layer as! AVPlayerLayer }
}

/// Counts what a display link sees come out of AVPlayer, and checks some of it against the pattern.
@MainActor
final class PresentationCounter: NSObject {
    let output: AVPlayerItemVideoOutput
    let clip: Clip
    private(set) var presented = 0
    private(set) var checked = 0
    private(set) var mismatched = 0
    private var link: CADisplayLink?

    init(output: AVPlayerItemVideoOutput, clip: Clip) {
        self.output = output
        self.clip = clip
    }

    func start() {
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    func stop() {
        link?.invalidate()
        link = nil
    }

    @objc private func tick(_ link: CADisplayLink) {
        let time = output.itemTime(forHostTime: link.targetTimestamp)
        guard output.hasNewPixelBuffer(forItemTime: time),
            let buffer = output.copyPixelBuffer(forItemTime: time, itemTimeForDisplay: nil)
        else { return }
        presented += 1
        guard presented % 15 == 0 else { return }
        // The composition repeats the clip, so a frame's number is its time within one repeat.
        let expected = Int((time.seconds * Double(clip.fps)).rounded()) % clip.frames
        checked += 1
        if PatternPixels.check(buffer, pattern: clip.pattern, expected: expected).frame == nil { mismatched += 1 }
    }
}

/// The clip played by AVPlayer in a visible layer, five times back to back in one composition: the system's
/// own decode, timing and presentation path. AVFoundation's access log counts the frames it dropped.
struct PlaybackTest: HWTest {
    let clip: Clip
    let subsystem = Subsystem.decode
    static let repeats = 5
    var id: String { "playback.\(clip.name)" }
    var name: String { "AVPlayer playback, \(VideoDecodeTest.codecName(clip.codec)) \(clip.label)" }

    func run(_ context: TestContext) async throws -> Outcome {
        let asset = AVURLAsset(url: context.media.appendingPathComponent(clip.file))
        let duration = try await asset.load(.duration)
        let composition = AVMutableComposition()
        for _ in 0..<Self.repeats {
            try await composition.insertTimeRange(
                CMTimeRange(start: .zero, duration: duration), of: asset, at: composition.duration)
        }
        // Finished: nothing mutates the composition once the player item owns it.
        nonisolated(unsafe) let timeline = composition
        let (item, output, player) = await MainActor.run { () -> (AVPlayerItem, AVPlayerItemVideoOutput, AVPlayer) in
            let item = AVPlayerItem(asset: timeline)
            let output = AVPlayerItemVideoOutput(pixelBufferAttributes: [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
            ])
            item.add(output)
            return (item, output, AVPlayer(playerItem: item))
        }
        let counter = await context.onStage { stage -> PresentationCounter in
            let view = PlayerView(frame: stage.bounds)
            view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            view.playerLayer.player = player
            view.playerLayer.videoGravity = .resizeAspect
            stage.addSubview(view)
            return PresentationCounter(output: output, clip: clip)
        }
        let ended = Waiter<Bool>()
        let observer = NotificationCenter.default.addObserver(
            forName: .AVPlayerItemDidPlayToEndTime, object: item, queue: .main
        ) { _ in ended.resume(true) }
        let failed = NotificationCenter.default.addObserver(
            forName: .AVPlayerItemFailedToPlayToEndTime, object: item, queue: .main
        ) { _ in ended.resume(false) }
        defer {
            NotificationCenter.default.removeObserver(observer)
            NotificationCenter.default.removeObserver(failed)
        }
        await MainActor.run {
            counter.start()
            player.play()
        }
        let seconds = duration.seconds * Double(Self.repeats)
        let finished = await ended.wait(timeout: seconds + 15)
        await MainActor.run {
            counter.stop()
            player.pause()
        }
        let events = item.accessLog()?.events ?? []
        let dropped = events.reduce(0) { $0 + max(0, $1.numberOfDroppedVideoFrames) }
        let expected = clip.frames * Self.repeats
        let (presented, checked, mismatched) = await MainActor.run {
            (counter.presented, counter.checked, counter.mismatched)
        }
        var metrics = Metrics()
        metrics.set("presented", presented)
        metrics.set("expected", expected)
        metrics.set("dropped", dropped)
        metrics.set("checked", checked)
        metrics.set("mismatched", mismatched)
        return .judged(
            [
                ("played to the end (\(item.error.map { String(describing: $0) } ?? "no error"))", finished == true),
                ("no frames dropped", dropped == 0),
                ("at least 95% of frames presented", Double(presented) >= 0.95 * Double(expected)),
                ("sampled frames match the pattern", checked > 0 && mismatched == 0),
            ], metrics)
    }

    static var all: [PlaybackTest] {
        Clip.all.filter { $0.height == 1080 && ($0.codec == .h264 || $0.codec == .hevc) }.map { PlaybackTest(clip: $0) }
    }
}
