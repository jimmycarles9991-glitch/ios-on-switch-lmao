import AVFoundation
import Foundation
import HWTestCore
import HWTestVideo
import UIKit

/// The landscape video route (drivers/NativeMobileDisplay/SwitchLandscape.h, debug kernels): the H.264 1080p clip full
/// screen, first on the shipped route (TegraVIC turns each frame into a portrait copy that window B scans), then with
/// V3's trait forced for this run's own TegraVIC connection, so TegraVIC records each turn and window A column-scans
/// the decoder's picture behind the UI on window B. DC A's CRC is read at every frame end of five samples: a held
/// frame on the copy, the change to landscape on that same frame, the clip playing, the held frame again, the change
/// back. Pass: no VIC job from Core Animation's display server for the landscape video (other processes' jobs are
/// reported, not judged), no frame of either change showing anything but one route's CRC or
/// the other's, the copy's very CRC back afterwards, no underflow, no frame dropped while playing. The two routes
/// filter differently (VIC scales the turn, window A's DDAs scale the scan), so their CRCs are reported (crc_same),
/// not compared.
struct LandscapeVideoTest: HWTest {
    let id = "display.x1.landscape_video"
    let name = "A landscape video column-scanned on window A under the UI on window B, against the VIC copy (CRC)"
    let subsystem = Subsystem.display
    var timeout: TimeInterval { 120 }

    static let heldFrame = CMTime(value: 30, timescale: 60)

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
        for _ in 0..<10 {
            try await composition.insertTimeRange(
                CMTimeRange(start: .zero, duration: duration), of: asset, at: composition.duration)
        }
        nonisolated(unsafe) let timeline = composition
        let (item, player) = await MainActor.run { () -> (AVPlayerItem, AVPlayer) in
            let item = AVPlayerItem(asset: timeline)
            return (item, AVPlayer(playerItem: item))
        }
        let video = player
        await context.onStage(fullScreen: true) { stage in
            let view = PlayerView(frame: stage.bounds)
            view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            view.playerLayer.player = video
            view.playerLayer.videoGravity = .resizeAspect
            stage.addSubview(view)
        }
        defer { _ = session.call(LandscapeSample.request(op: LandscapeSample.force, value: 0), replyBytes: 0) }
        await MainActor.run { video.play() }
        try? await Task.sleep(nanoseconds: 1_000_000_000)

        var samples = LandscapeJudge.Samples()
        await hold(video)
        guard case .success(let vicHeld) = await sample(session, frames: 30) else { return .fail("no VIC sample") }
        samples.vicHeld = vicHeld
        _ = session.call(
            LandscapeSample.request(op: LandscapeSample.force, value: LandscapeSample.columnScanYuv), replyBytes: 0)
        guard start(session, frames: 90) == nil else { return .fail("no sample of the change") }
        await hold(video, settle: false)
        guard case .success(let enter) = await finish(session, frames: 90) else {
            return .fail("no sample of the change")
        }
        samples.enter = enter
        let droppedBefore = Self.dropped(item)
        await MainActor.run { video.play() }
        guard case .success(let playing) = await sample(session, frames: 180) else {
            return .fail("no playing sample")
        }
        let dropped = Self.dropped(item) - droppedBefore
        samples.playing = playing
        await hold(video)
        guard case .success(let landscapeHeld) = await sample(session, frames: 30) else {
            return .fail("no held landscape sample")
        }
        samples.landscapeHeld = landscapeHeld
        _ = session.call(LandscapeSample.request(op: LandscapeSample.force, value: 0), replyBytes: 0)
        guard start(session, frames: 90) == nil else { return .fail("no sample of the change back") }
        await hold(video, settle: false)
        guard case .success(let leave) = await finish(session, frames: 90) else {
            return .fail("no sample of the change back")
        }
        samples.leave = leave
        await MainActor.run { video.pause() }

        let (checks, metrics) = LandscapeJudge.judge(samples, dropped: dropped)
        return .judged(checks, metrics)
    }

    /// The frames AVFoundation has dropped so far for `item`.
    static func dropped(_ item: AVPlayerItem) -> Int {
        (item.accessLog()?.events ?? []).reduce(0) { $0 + max(0, $1.numberOfDroppedVideoFrames) }
    }

    /// Paused on the held frame, decoded again (a seek with no tolerance), so the next swap carries a new turn of it.
    func hold(_ video: AVPlayer, settle: Bool = true) async {
        let sought = Waiter<Bool>()
        await MainActor.run {
            video.pause()
            video.seek(to: Self.heldFrame, toleranceBefore: .zero, toleranceAfter: .zero) { sought.resume($0) }
        }
        _ = await sought.wait(timeout: 5)
        if settle { try? await Task.sleep(nanoseconds: 500_000_000) }
    }

    func sample(_ session: VICKernelTest.Session, frames: UInt32) async -> Result<LandscapeSample, TestFailure> {
        if let failure = start(session, frames: frames) { return .failure(failure) }
        return await finish(session, frames: frames)
    }

    /// Has the kernel record the next `frames` frame ends; nil once it does.
    func start(_ session: VICKernelTest.Session, frames: UInt32) -> TestFailure? {
        switch session.call(
            LandscapeSample.request(op: LandscapeSample.sample, value: frames), replyBytes: LandscapeSample.size)
        {
        case .failure(let failure): return failure
        case .success(let bytes):
            guard let started = LandscapeSample(bytes), started.error == 0 else {
                return TestFailure("sample not started (errno \(LandscapeSample(bytes)?.error ?? -1))")
            }
            return nil
        }
    }

    /// The sample once the kernel has all of it (a frame end every 16.7 ms).
    func finish(_ session: VICKernelTest.Session, frames: UInt32) async -> Result<LandscapeSample, TestFailure> {
        let size = LandscapeSample.size
        for _ in 0..<Int(frames / 6 + 60) {
            try? await Task.sleep(nanoseconds: 100_000_000)
            guard
                case .success(let bytes) = session.call(
                    LandscapeSample.request(op: LandscapeSample.query, value: 0), replyBytes: size),
                let reply = LandscapeSample(bytes)
            else { return .failure(TestFailure("malformed reply")) }
            if reply.state == LandscapeSample.done { return .success(reply) }
        }
        return .failure(TestFailure("the sample did not finish"))
    }
}
