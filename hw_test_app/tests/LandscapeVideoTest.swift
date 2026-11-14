import AVFoundation
import Foundation
import HWTestCore
import HWTestVideo
import UIKit

/// The landscape video route (drivers/NativeMobileDisplay/SwitchLandscape.h, debug kernels): the H.264 1080p clip full
/// screen, first on the copy route (TegraVIC turns each frame into a portrait copy that window B scans), held there by
/// PORTRAIT whatever the shipped traits, then on the landscape route (V3's trait shipped, and FORCEd for this run's own
/// TegraVIC connection on kernels without it): TegraVIC records each turn and window A column-scans the decoder's picture
/// behind the UI on window B. DC A's CRC is read at every frame end of five samples: a held frame on the copy, the
/// change to landscape on that same frame, the clip playing, the held frame again, the change back. A change into
/// landscape takes a new turn, so the held frame is decoded again from the clip's next copy in the composition; the
/// change back is asked on the frame already shown, as boots 19 and 21 did (decoded again from the third sample), so the
/// copy route has to show a frame whose turn TegraVIC only recorded: crc_back has to be crc_vic. A change sample is
/// taken again until it shows the change (enter_samples, leave_samples): a change that never lands is the route's
/// fault, one that lands late only the sampling's. Pass: no VIC job from Core Animation's display server for the landscape
/// video (other processes' jobs are reported, not judged), no frame of either change showing anything but one route's
/// CRC or the other's (the landscape route's CRC is the held landscape frame's), the copy's very CRC back afterwards,
/// no underflow, no frame dropped while playing. The two routes filter differently (VIC scales the turn, window A's
/// DDAs scale the scan), so their CRCs are reported (crc_same), not compared. Last, the forced-failure arm.
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
        defer {
            _ = session.call(LandscapeSample.request(op: LandscapeSample.force, value: 0), replyBytes: 0)
            _ = session.call(LandscapeSample.request(op: LandscapeSample.portrait, value: 0), replyBytes: 0)
            _ = session.call(LandscapeSample.request(op: LandscapeSample.fail, value: 0), replyBytes: 0)
        }
        let held = Held(frame: Self.heldFrame, clip: duration)
        _ = session.call(
            LandscapeSample.request(op: LandscapeSample.portrait, value: LandscapeSample.portraitFrames), replyBytes: 0)
        await MainActor.run { video.play() }
        try? await Task.sleep(nanoseconds: 1_000_000_000)

        var samples = LandscapeJudge.Samples()
        await held.show(video)
        guard case .success(let vicHeld) = await sample(session, frames: 30) else { return .fail("no VIC sample") }
        samples.vicHeld = vicHeld
        _ = session.call(
            LandscapeSample.request(op: LandscapeSample.force, value: LandscapeSample.columnScanYuv), replyBytes: 0)
        _ = session.call(LandscapeSample.request(op: LandscapeSample.portrait, value: 0), replyBytes: 0)
        guard case .success(let (enter, enterSamples)) = await change(session, video, held) else {
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
        await held.show(video)
        guard case .success(let landscapeHeld) = await sample(session, frames: 30) else {
            return .fail("no held landscape sample")
        }
        samples.landscapeHeld = landscapeHeld
        _ = session.call(
            LandscapeSample.request(op: LandscapeSample.portrait, value: LandscapeSample.portraitFrames), replyBytes: 0)
        guard case .success(let (leave, leaveSamples)) = await change(session, video, held, fresh: false) else {
            return .fail("no sample of the change back")
        }
        _ = session.call(LandscapeSample.request(op: LandscapeSample.portrait, value: 0), replyBytes: 0)
        samples.leave = leave
        let (forcedChecks, forcedMetrics) = await forcedFailure(session, video, held)
        await MainActor.run { video.pause() }

        var (checks, metrics) = LandscapeJudge.judge(samples, dropped: dropped)
        metrics.set("enter_samples", enterSamples)
        metrics.set("leave_samples", leaveSamples)
        metrics.merge(forcedMetrics)
        checks += forcedChecks
        return .judged(checks, metrics)
    }

    /// The forced-failure arm, last: FAIL makes the next change into landscape fail as a reservation would (one line,
    /// that change dropped); nothing is held, so a following frame's change has to go landscape. forced_failures is the
    /// kernel's count over the arm.
    func forcedFailure(
        _ session: VICKernelTest.Session, _ video: AVPlayer, _ held: Held
    ) async
        -> ([(String, Bool)], Metrics)
    {
        var metrics = Metrics()
        guard case .success(let before) = await sample(session, frames: 2) else {
            return ([("the forced-failure arm sampled", false)], metrics)
        }
        _ = session.call(
            LandscapeSample.request(op: LandscapeSample.force, value: LandscapeSample.columnScanYuv), replyBytes: 0)
        _ = session.call(LandscapeSample.request(op: LandscapeSample.fail, value: 1), replyBytes: 0)
        await MainActor.run { video.play() }
        var entered = false
        var last = before
        for _ in 0..<6 {
            guard case .success(let window) = await sample(session, frames: 120) else { break }
            last = window
            if window.failures > before.failures && window.landscapeFrames > 0 {
                entered = true
                break
            }
        }
        _ = session.call(LandscapeSample.request(op: LandscapeSample.force, value: 0), replyBytes: 0)
        _ = session.call(
            LandscapeSample.request(op: LandscapeSample.portrait, value: LandscapeSample.portraitFrames), replyBytes: 0)
        await held.show(video)
        _ = session.call(LandscapeSample.request(op: LandscapeSample.portrait, value: 0), replyBytes: 0)
        metrics.set("forced_failures", last.failures - before.failures)
        metrics.set("forced_entered", entered)
        return (
            [
                ("the forced failure dropped one change (forced_failures 1)", last.failures - before.failures == 1),
                ("a following frame went landscape (forced_entered)", entered),
            ], metrics
        )
    }

    /// The frames AVFoundation has dropped so far for `item`.
    static func dropped(_ item: AVPlayerItem) -> Int {
        (item.accessLog()?.events ?? []).reduce(0) { $0 + max(0, $1.numberOfDroppedVideoFrames) }
    }

    /// The held frame: the same picture in each of the composition's ten copies of the clip. A fresh show seeks to the
    /// copy it is not at, so the frame is decoded and turned again and the next swap carries a new turn of it (a seek to
    /// the time already shown decodes nothing, and a change into landscape waits for a swap with a recorded turn); a
    /// show that is not fresh seeks to the time already shown, as boots 19 and 21 did.
    final class Held: @unchecked Sendable {
        let frame: CMTime
        let clip: CMTime

        init(frame: CMTime, clip: CMTime) {
            self.frame = frame
            self.clip = clip
        }

        func show(_ video: AVPlayer, settle: Bool = true, fresh: Bool = true) async {
            let sought = Waiter<Bool>()
            let first = frame, second = CMTimeAdd(frame, clip)
            await MainActor.run {
                video.pause()
                let now = video.currentTime()
                let atFirst =
                    CMTimeCompare(CMTimeAbsoluteValue(CMTimeSubtract(now, first)), CMTime(value: 1, timescale: 60)) < 0
                let to = atFirst == fresh ? second : first
                video.seek(to: to, toleranceBefore: .zero, toleranceAfter: .zero) { sought.resume($0) }
            }
            _ = await sought.wait(timeout: 5)
            if settle { try? await Task.sleep(nanoseconds: 500_000_000) }
        }
    }

    /// A change of mode on the held frame: a sample started, the frame shown again (fresh, or first at the time already
    /// shown and fresh from the third sample), and more samples until one shows the change latched (switches) or
    /// Self.changeSamples are spent; the sample the change landed in (or the last), and how many it took.
    static let changeSamples = 4

    func change(
        _ session: VICKernelTest.Session, _ video: AVPlayer, _ held: Held, fresh: Bool = true
    ) async
        -> Result<(LandscapeSample, Int), TestFailure>
    {
        var last = LandscapeSample()
        for attempt in 1...Self.changeSamples {
            if let failure = start(session, frames: 90) { return .failure(failure) }
            if attempt == 1 { await held.show(video, settle: false, fresh: fresh) }
            if attempt == 3 && !fresh { await held.show(video, settle: false) }
            switch await finish(session, frames: 90) {
            case .failure(let failure): return .failure(failure)
            case .success(let window):
                last = window
                if window.switches >= 1 { return .success((window, attempt)) }
            }
        }
        return .success((last, Self.changeSamples))
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
