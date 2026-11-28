import Darwin
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import Metal
import QuartzCore
import UIKit

/// presentDrawable:atTime: three periods past the display link's next frame, every fourth frame for three seconds, on a
/// CAMetalLayer the stage's size (judged: PresentTimeVerdict). Controls follow at the same size and cadence, each
/// "shown/submitted" with its plane flips and median latency: on fresh layers for a second each, plain presents
/// (plain=), a time already past (past=) and four periods after CACurrentMediaTime() (media=); then warm=, the timed
/// presents after half a second of plain ones on one layer, and mlink=, normal presents from a CAMetalDisplayLink
/// driving a fresh layer for two seconds (warm_off_ms, mlink_off_ms: median presentedTime minus target). A failure so
/// says whether timed presents, future times, a layer Core Animation has not shown yet, the kind of display link or
/// this layer's route lose presentedTime. offsets_ms= is each timed drawable's presentedTime minus its target ("x":
/// none); lead_min_ms its commit to target; hlag_* when its handler ran against its target, hc_p50_ms (and each
/// control's <key>_hc_ms) against its commit: a presented handler within a few ms of its commit, with no
/// presentedTime, is a drawable QuartzCore released at presentAtTime: without queueing it. d_fi_* (debug kernels)
/// count the display's swaps: held for their time and shown or replaced unsent, shown more than half a period before
/// their requested presentation, and let go as the render server's ordinary frame with the frame ends masked. Two
/// phases settle why timed drawables come back with no time (QuartzCore 24B119 finishes every drawable
/// of a layer that had no presented report for a second, the one presented now included): alt_plain= and alt_timed=,
/// plain and timed presents in turn on one layer (timed ones shown here: the server shows and reports them, and what
/// fails alone is the layer's report going stale; none: the server never takes or never reports a timed image); and
/// sw_<kind>=, a sweep of targets (PresentMode.sweepTargets: none shown at any time, past ones included, says the time
/// is not what is refused).
struct PresentAtTimeTest: HWTest {
    let id = "display.present_at_time"
    let name = "presentDrawable:atTime: shows each drawable at its refresh, with its presentedTime"
    let subsystem = Subsystem.compositor

    static let counters = ["shown", "unsent", "early", "unphased"]

    func run(_ context: TestContext) async throws -> Outcome {
        let kernelBefore = Self.frameInfoTotals()
        let paths = CommandPaths()
        let timed = try await phase(context, .linkTarget, seconds: 3, paths: paths)
        let plain = try await phase(context, .plain, seconds: 1, paths: paths)
        let past = try await phase(context, .past, seconds: 1, paths: paths)
        let media = try await phase(context, .mediaTarget, seconds: 1, paths: paths)
        let warm = try await phase(context, .warm, seconds: 1.5, paths: paths)
        let metalLink = try await phase(context, .metalLink, seconds: 2, paths: paths)
        let alternate = try await phase(context, .alternate, seconds: 2, paths: paths)
        let sweep = try await phase(context, .sweep, seconds: 2.5, paths: paths)
        let kernelAfter = Self.frameInfoTotals()

        let frames = timed.frames
        let summary = PresentTimeVerdict.Summary(frames)
        let offsets = frames.compactMap(\.offsetMs)
        let handlerLag = frames.filter { $0.handled > 0 }.map { ($0.handled - $0.target) * 1000 }
        let handlerAfterCommit = { (frames: [PresentTimeVerdict.Frame]) in
            Stats.percentile(frames.filter { $0.handled > 0 }.map { ($0.handled - $0.committed) * 1000 }, 0.5)
        }
        var metrics = Metrics()
        metrics.set("submitted", summary.submitted)
        metrics.set("timed", summary.timed)
        metrics.set("handled", summary.handled)
        metrics.set("early", summary.early)
        metrics.set("late", summary.late)
        metrics.set("lead_min_ms", summary.leadMinMs, digits: 1)
        metrics.set("offset_p50_ms", Stats.percentile(offsets, 0.5), digits: 1)
        metrics.set("hlag_min_ms", handlerLag.min() ?? 0, digits: 1)
        metrics.set("hlag_p50_ms", Stats.percentile(handlerLag, 0.5), digits: 1)
        metrics.set("hc_p50_ms", handlerAfterCommit(frames), digits: 1)
        metrics.set("drawable", timed.drawable)
        metrics.set("d_flips", timed.flips)
        for name in Self.counters {
            if let before = kernelBefore[name], let after = kernelAfter[name] {
                metrics.set("d_fi_" + name, after - before)
            }
        }
        metrics.set("asleep_ms", Self.asleepMs(), digits: 0)
        let controls = [("plain", plain), ("past", past), ("media", media), ("warm", warm), ("mlink", metalLink)]
        for (key, control) in controls {
            metrics.set(key, PresentTimeVerdict.shown(control.frames))
            metrics.set(key + "_flips", control.flips)
            let latency = control.frames.compactMap(\.latencyMs)
            metrics.set(key + "_lat_ms", Stats.percentile(latency, 0.5), digits: 1)
            metrics.set(key + "_hc_ms", handlerAfterCommit(control.frames), digits: 1)
        }
        metrics.set("alt_plain", PresentTimeVerdict.shown(alternate.frames.filter { $0.target == 0 }))
        metrics.set("alt_timed", PresentTimeVerdict.shown(alternate.frames.filter { $0.target > 0 }))
        metrics.set("alt_timed_hc_ms", handlerAfterCommit(alternate.frames.filter { $0.target > 0 }), digits: 1)
        for kind in PresentMode.sweepTargets.map(\.name) {
            metrics.set("sw_" + kind, PresentTimeVerdict.shown(sweep.frames.filter { Self.sweepKind($0) == kind }))
        }
        for (key, control) in [("warm", warm), ("mlink", metalLink)] {
            metrics.set(key + "_off_ms", Stats.percentile(control.frames.compactMap(\.offsetMs), 0.5), digits: 1)
        }
        metrics.set("offsets_ms", PresentTimeVerdict.offsets(frames))
        metrics.fitLog(within: 760, keeping: ["submitted", "timed", "handled", "early", "late", "offsets_ms"])
        let linked = PresentTimeVerdict.Summary(metalLink.frames)
        let checks =
            PresentTimeVerdict.checks(frames, minimum: 40) + [
                ("metal_link_submitted", linked.submitted > 0),
                ("metal_link_handlers", linked.handled == linked.submitted),
                ("metal_link_presented_times", linked.timed == linked.submitted),
            ]
        return .judged(checks, metrics)
            .onRawPath(paths, device: MTLCreateSystemDefaultDevice())
    }

    struct PhaseResult {
        let frames: [PresentTimeVerdict.Frame]
        let drawable: String
        let flips: Int
    }

    /// One phase on a fresh layer the size the stage has when the test builds it (as boots 19-22 ran it), and window
    /// B's flips meanwhile (-1 when kern.switch_planes_flips cannot be read).
    private func phase(
        _ context: TestContext, _ mode: PresentMode, seconds: Double, paths: CommandPaths
    ) async throws
        -> PhaseResult
    {
        let flipsBefore = PlanesTest.totals(["flips"]).values["flips"]
        let done = Waiter<Bool>()
        let run = await context.onStage(fullScreen: true) { stage -> PresentAtTimeRun in
            let run = PresentAtTimeRun(mode: mode, seconds: seconds, done: done, paths: paths)
            run.layer.frame = stage.bounds
            run.layer.contentsScale = UIScreen.main.scale
            stage.layer.addSublayer(run.layer)
            return run
        }
        await MainActor.run { run.start() }
        _ = await done.wait(timeout: seconds + 5)
        let presented = await MainActor.run { run.everyPresented() }
        _ = await presented.wait(timeout: 0.3)
        let flipsAfter = PlanesTest.totals(["flips"]).values["flips"]
        let (frames, size) = await MainActor.run { (run.results, run.drawableSize) }
        let flips = flipsBefore.flatMap { before in flipsAfter.map { $0 - before } } ?? -1
        return PhaseResult(frames: frames, drawable: "\(Int(size.width))x\(Int(size.height))", flips: flips)
    }

    /// Which of PresentMode.sweepTargets a drawable of the sweep asked for.
    static func sweepKind(_ frame: PresentTimeVerdict.Frame) -> String {
        if frame.target < 0.0005 { return "floor" }
        if frame.target < 0.01 { return "over" }
        if frame.target < 2 { return "boot" }
        if frame.target < frame.committed - 5 { return "past10" }
        return frame.target < frame.committed ? "past01" : "ahead"
    }

    /// kern.switch_frame_info_* (debug kernels) by name; a counter that cannot be read is left out.
    static func frameInfoTotals() -> [String: Int] {
        var values: [String: Int] = [:]
        for name in counters {
            if case .value(let value) = KernelSysctl.integer("kern.switch_frame_info_" + name, as: Int32.self) {
                values[name] = Int(value)
            }
        }
        return values
    }

    /// mach_continuous_time() ahead of mach_absolute_time(): the time asleep since boot, in milliseconds. The absolute
    /// clock is read first: read second, it is ahead of a continuous clock that never slept.
    static func asleepMs() -> Double {
        var base = mach_timebase_info_data_t()
        mach_timebase_info(&base)
        let absolute = mach_absolute_time()
        let continuous = mach_continuous_time()
        let ticks = Double(continuous > absolute ? continuous - absolute : 0)
        return ticks * Double(base.numer) / Double(base.denom) / 1_000_000
    }
}
