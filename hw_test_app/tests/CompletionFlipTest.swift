import Foundation
import HWTestCore
import UIKit

/// A direct frame's window A flip is written by its producer's own completion (the GPU's interrupt pass for a watched
/// present, the present job's completion otherwise), with no hop to the display's work loop, which takes the flip
/// into its ring afterwards (drivers/NativeMobileDisplay/SwitchCompletionFlipDisplay.h). Runs display.pacing's
/// full-screen colour change for 5 s and reads the kernel's window A flip counters around it (kern.switch_flips_*,
/// debug kernels): completions must have written flips, each ahead of the work loop, with the frames still paced. A
/// watched present's and a VIC copy's flip comes earlier still, from the producer's primary interrupt filter (filter,
/// filter_lead_us: how far ahead of its completion); filter_share is their part of window A's flips.
/// refused is the boot's total: a unit whose window A aperture does not latch the completion's stores turns completion
/// flips off at the first one (one SwitchDisplay line), so a FAIL with refused=1 and d_completion=0 is that verdict.
/// The compositor's own frames behind a writer promise (its command buffer held on a shared event) take the direct
/// present's promise watch: kern.switch_gpu_promises' direct_* totals count them (direct_promised, direct_kept), and
/// none may be given up (direct_given_up 0 since boot): a frame is given up only when its writer never runs.
struct CompletionFlipTest: HWTest {
    let id = "display.completion_flip"
    let name = "Window A flips written by their producers' completions"
    let subsystem = Subsystem.display

    static let counters = [
        "completion", "gate", "lead_us", "withdrawn", "refused", "filter", "filter_lead_us", "filter_declined",
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let (before, unread) = Self.totals()
        let promisesBefore = try? PromiseTotals.read()
        guard !before.isEmpty else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(unread))
            return .skip(KernelSysctl.unreadable("kern.switch_flips_*", unread), metrics)
        }
        let done = Waiter<Bool>()
        let recorder = await context.onStage(fullScreen: true) { stage -> PacingRecorder in
            let view = UIView(frame: stage.bounds)
            view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            stage.addSubview(view)
            return PacingRecorder(view: view, seconds: 5, done: done)
        }
        await MainActor.run { recorder.start() }
        _ = await done.wait(timeout: 15)
        let stamps = await MainActor.run { recorder.timestamps }
        let after = Self.totals().values
        let promisesAfter = try? PromiseTotals.read()
        let delta = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }
        let intervals = zip(stamps.dropFirst(), stamps).map { ($0 - $1) * 1000 }
        let completion = delta("completion")
        let flips = completion + delta("gate")

        let refused = after["refused"] ?? 0

        var metrics = Metrics()
        for name in Self.counters { metrics.set("d_" + name, delta(name)) }
        metrics.set("refused", refused)
        metrics.set("frames", stamps.count)
        metrics.set("completion_share", flips > 0 ? Double(completion) / Double(flips) : 0, digits: 3)
        metrics.set("lead_mean_us", completion > 0 ? Double(delta("lead_us")) / Double(completion) : 0, digits: 1)
        let filter = delta("filter")
        metrics.set("filter_share", flips > 0 ? Double(filter) / Double(flips) : 0, digits: 3)
        metrics.set("filter_lead_mean_us", filter > 0 ? Double(delta("filter_lead_us")) / Double(filter) : 0, digits: 1)
        metrics.set("dropped", Stats.droppedFrames(intervals: intervals, period: 1000.0 / 60))
        var promised: [(String, Bool)] = []
        if let promisesBefore, let promisesAfter {
            metrics.set("d_direct_promised", promisesAfter.directPromised - promisesBefore.directPromised)
            metrics.set("d_direct_kept", promisesAfter.directKept - promisesBefore.directKept)
            metrics.set("direct_given_up", promisesAfter.directGivenUp)
            promised.append(
                (
                    "no promised direct present given up since boot (direct_given_up = 0)",
                    promisesAfter.directGivenUp == 0
                ))
        }
        return .judged(
            [
                ("window A latched every completion flip as written (refused = 0)", refused == 0),
                ("5 s of frames", stamps.count >= 290),
                ("completions flipped window A (d_completion > 0)", completion > 0),
                ("ahead of the display's work loop (d_lead_us > 0)", delta("lead_us") > 0),
            ] + promised, metrics)
    }

    /// kern.switch_flips_* by name, and the errno of the first that could not be read.
    static func totals() -> (values: [String: Int], unread: Int32) {
        var values: [String: Int] = [:]
        var unread: Int32 = 0
        for name in counters {
            switch KernelSysctl.integer("kern.switch_flips_" + name, as: Int32.self) {
            case .value(let value): values[name] = Int(value)
            case .failed(let error): if unread == 0 { unread = error }
            }
        }
        return (values, unread)
    }
}
