import Foundation
import HWTestCore
import HWTestGPU
import QuartzCore
import UIKit

/// The person turns the display off with the power button and back on, returning to this app. With the panel dark
/// the kernel rail-gates the GPU; this measures how long from the app becoming active again to the first frame the
/// display showed (wake_ms, a probe Metal layer's presented time), the pacing of the two seconds after, and runs the
/// first-surface GPU scene again on the GPU that was just powered back up.
struct WakeTest: HWTest {
    let id = "display.wake_gpu"
    let name = "GPU and first frame after the display sleeps and wakes"
    let subsystem = Subsystem.display
    let interactive = true

    var timeout: TimeInterval { 150 }

    func run(_ context: TestContext) async throws -> Outcome {
        let observers = await MainActor.run { WakeObservers() }
        let active = await context.interact(
            "Press the power button to turn the screen off, wait 5 seconds, wake it and come back to this app",
            timeout: 120
        ) { (waiter: Waiter<CFTimeInterval>) in observers.watch(waiter) }
        await MainActor.run { observers.stop() }
        guard let active else { return .skip("the display did not sleep and wake (or Skip)") }
        let done = Waiter<Bool>()
        let recorder = await context.onStage(fullScreen: true) { stage -> PacingRun in
            let recorder = PacingRun(seconds: 2, animate: { _ in }, done: done)
            recorder.probe.frame = UIScreen.main.bounds
            stage.layer.addSublayer(recorder.probe)
            return recorder
        }
        await MainActor.run { recorder.start() }
        _ = await done.wait(timeout: 10)
        try await Task.sleep(nanoseconds: 200_000_000)
        let (first, presented) = await MainActor.run { (recorder.firstPresented, recorder.presented) }
        await context.clearStage()
        let shown = Pacing(times: presented.sorted(), period: 1.0 / 60)
        var metrics = Metrics()
        metrics.set("wake_ms", first > 0 ? (first - active) * 1000 : -1, digits: 1)
        metrics.merge(shown.metrics(prefix: "present_"))
        let gpu = try await GPUSceneTest(scene: Scenes.firstSurface).run(context)
        metrics.merge(gpu.metrics, prefix: "gpu_")
        return .judged(
            [
                ("a frame shown after waking", first > 0),
                ("the first frame within 500 ms of becoming active", first > 0 && first - active <= 0.5),
                ("no frame missed after waking", shown.frames > 0 && shown.missed == 0),
                ("the GPU scene after waking: \(gpu.detail)", gpu.verdict == Verdict.pass),
            ], metrics)
    }
}

/// Resolves with the media time the app became active again after it went to the background.
@MainActor
final class WakeObservers {
    private var tokens: [NSObjectProtocol] = []
    private var left = false

    func watch(_ waiter: Waiter<CFTimeInterval>) {
        let center = NotificationCenter.default
        tokens.append(
            center.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) {
                [weak self] _ in MainActor.assumeIsolated { self?.left = true }
            })
        tokens.append(
            center.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) {
                [weak self] _ in
                MainActor.assumeIsolated {
                    if self?.left == true { waiter.resume(CACurrentMediaTime()) }
                }
            })
    }

    func stop() {
        tokens.forEach(NotificationCenter.default.removeObserver)
        tokens = []
    }
}
