import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import QuartzCore
import UIKit

/// The owner's "the title blur hitches every time": the main screen's list scrolled by program under its large title
/// (TitleScroller) so the title collapses, its progressive blur and text fade appear, and it opens again; four
/// repetitions each after a pause of 1.5 s with the screen still (paused), then four back to back (back). Per
/// repetition, from the frame probe in the window's corner: the shown frames' intervals (worst, late frames and their
/// phases on the 250 ms grid of absolute time, probe frames with no presented time past the first, which QuartzCore
/// finishes unshown after a pause), and from the debug kernel's compositor notes (TegraGM20BPathsABI.h spans op) the
/// render server's longest GPU job and its spans of Core Animation's catch-all states (5 ms or more). Every repetition
/// after the first must have no late frame, no unshown frame and no catch-all span; each must have collapsed the title
/// (the bar's height moved by 20 pt or more) and each paused one must have had a still screen (no window A flip
/// during its pause: kern.switch_flips_completion and _gate). State creations, catch-all draws under 5 ms, fresh
/// imports, zeroed bytes and object-call time are the render server's and reach no counter the app can read: each
/// repetition's SwitchMark line names its span for tools/perf and tools/devicerun to read them from backboardd's lines.
struct TitleScrollTest: HWTest {
    let id = "ui.title_scroll"
    let name = "Scrolling the main list under its large title hitches only the first time"
    let subsystem = Subsystem.compositor
    var timeout: TimeInterval { 60 }

    static let repetitions = 4, pauseSeconds = 1.5, scrollSeconds = 0.5
    static let depth: CGFloat = 320, collapse: CGFloat = 20, period = 1.0 / 60

    struct Repetition {
        let arm: String
        let index: Int
        let cycle: TitleScroller.Cycle
        let late: LateFrames
        let unshown: Int
        let worstMs: Double
        let pauseFlips: Int?
        let pauseMs: Double
        let generic: Int
        let jobMaxUs: Int?
    }

    func run(_ context: TestContext) async throws -> Outcome {
        await context.clearStage()
        guard let scroller = await MainActor.run(body: { TitleScroller.find() }) else {
            return .skip("the main screen has no scroll view")
        }
        let probe = await MainActor.run { () -> FrameProbe in
            let bounds = scroller.window.bounds
            let probe = FrameProbe(frame: CGRect(x: bounds.width - 58, y: bounds.height - 58, width: 48, height: 48))
            scroller.window.layer.addSublayer(probe.layer)
            return probe
        }
        let gpu = GPUConnection()
        let states = Self.genericStates()
        _ = gpu.flatMap { LaunchHitchTest.drain($0) }
        var reps: [Repetition] = []
        for arm in ["paused", "back"] {
            for index in 0 ..< Self.repetitions {
                var flips: Int?
                var pauseMs = 0.0
                if arm == "paused" {
                    let before = Self.flips(), paused = CACurrentMediaTime()
                    await Self.pause(Self.pauseSeconds)
                    pauseMs = (CACurrentMediaTime() - paused) * 1000
                    flips = zip(before, Self.flips()).map { $1 - $0 }.reduce(0, +)
                    if before.isEmpty { flips = nil }
                }
                probe.start()
                let cycle = await scroller.run(depth: Self.depth, seconds: Self.scrollSeconds)
                await probe.stop()
                let reading = gpu.flatMap { LaunchHitchTest.drain($0) }
                let verdict = reading.map { LaunchHitchVerdict(reading: $0, states: states, jobBoundUs: 0) }
                reps.append(Self.repetition(arm, index, cycle, probe, flips, pauseMs, verdict))
            }
        }
        await MainActor.run {
            scroller.restore()
            probe.layer.removeFromSuperlayer()
        }
        return Self.verdict(reps, spans: gpu != nil)
    }

    static func repetition(
        _ arm: String, _ index: Int, _ cycle: TitleScroller.Cycle, _ probe: FrameProbe, _ flips: Int?,
        _ pauseMs: Double,
        _ verdict: LaunchHitchVerdict?
    ) -> Repetition {
        let shown = probe.presented(from: cycle.start, to: cycle.end)
        let late = LateFrames(presented: shown, period: period)
        let times = shown.filter { $0 > 0 }.sorted()
        let worst = zip(times, times.dropFirst()).map { ($1 - $0) * 1000 }.max() ?? 0
        let rep = Repetition(
            arm: arm, index: index, cycle: cycle, late: late, unshown: shown.dropFirst().filter { $0 <= 0 }.count,
            worstMs: worst, pauseFlips: flips, pauseMs: pauseMs, generic: verdict?.generic.count ?? 0,
            jobMaxUs: verdict?.reading.jobMaxUs
        )
        SwitchMarks.mark(
            "ui.title_scroll.\(arm).\(index)", since: cycle.start,
            "frames=\(shown.count) worst_ms=" + String(format: "%.1f", worst) + " late=\(late.late.count) "
                + "phases=\(late.phases()) unshown=\(rep.unshown) bar=\(Int(cycle.barMin))-\(Int(cycle.barMax)) "
                + "pause_ms=\(Int(pauseMs)) pause_flips=\(flips.map(String.init) ?? "-") generic=\(rep.generic) "
                + "job_max_us=\(rep.jobMaxUs.map(String.init) ?? "-")"
        )
        return rep
    }

    static func verdict(_ reps: [Repetition], spans: Bool) -> Outcome {
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for rep in reps {
            let key = (rep.arm == "paused" ? "p" : "b") + "\(rep.index)"
            metrics.set(key + "_worst_ms", rep.worstMs, digits: 1)
            metrics.set(key + "_late", rep.late.late.count)
            metrics.set(key + "_unshown", rep.unshown)
            metrics.set(key + "_generic", rep.generic)
            let collapsed = rep.cycle.barMax - rep.cycle.barMin >= collapse && rep.cycle.deepest >= depth - 1
            checks.append(("\(key): the title collapsed (bar \(rep.cycle.barMin)-\(rep.cycle.barMax) pt)", collapsed))
            if let flips = rep.pauseFlips {
                checks.append(("\(key): the screen kept still in its pause (\(flips) window A flips)", flips == 0))
            }
            guard rep.arm != "paused" || rep.index > 0 else { continue }
            checks += [
                ("\(key): \(rep.late.late.count) late frames (\(rep.late.phases()))", rep.late.late.isEmpty),
                ("\(key): \(rep.unshown) probe frames with no presented time", rep.unshown == 0),
                ("\(key): \(rep.generic) catch-all spans", rep.generic == 0),
            ]
        }
        metrics.set("job_max_us", reps.compactMap(\.jobMaxUs).max() ?? -1)
        metrics.set("spans", spans)
        metrics.set("pause_flips", reps.compactMap(\.pauseFlips).map(String.init).joined(separator: ","))
        return .judged(checks, metrics)
    }

    /// The pause, the stimulus of the paused arm: one wait for its length.
    static func pause(_ seconds: Double) async {
        let done = Waiter<Bool>()
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { done.resume(true) }
        _ = await done.wait(timeout: seconds + 2)
    }

    /// Window A's flips so far, as its completions and its gate wrote them; empty when the kernel has neither.
    static func flips() -> [Int] {
        let names = ["kern.switch_flips_completion", "kern.switch_flips_gate"]
        let reads = names.map { KernelSysctl.integer($0, as: Int32.self) }
        let values = reads.compactMap { read -> Int? in
            if case .value(let value) = read { return Int(value) }
            return nil
        }
        return values.count == names.count ? values : []
    }

    static func genericStates() -> GenericStates {
        guard let url = Bundle.main.url(forResource: "ca_generic_keys", withExtension: "txt"),
              let table = try? String(contentsOf: url, encoding: .utf8)
        else { return GenericStates(names: [:]) }
        return GenericStates.parse(table)
    }
}
