import Darwin
import Foundation
import HWTestCore
import QuartzCore
import UIKit

/// The owner's report that a plain busy loop drops the frame rate: 1, 2, 4 and 8 threads spinning at default QoS, then
/// at user-interactive QoS, beside pacing.text's scene (labels redrawn every frame), against a baseline before the
/// first arm and after the last. Each arm's window is two seconds after a warm-up. The scene's display link on the main
/// thread is the app's responsiveness (frames it missed, callbacks that began after their frame's target), the frame
/// probe on a thread of its own the compositor's and the display's frames. An arm passes when it is no worse than the
/// worse baseline by more than one missed frame or two late callbacks, and every spinner it started ran. Per arm:
/// <arm>_pm present missed, _lm link missed, _late, _cpu the least CPU share of its spinners in percent; each arm's
/// SwitchMark line has its histograms, late-frame phases and each spinner's CPU time.
struct HogPacingTest: HWTest {
    let id = "cpu.hog_pacing"
    let name = "Busy threads at default and user-interactive QoS leave the frame rate and the main thread unchanged"
    let subsystem = Subsystem.cpu
    var timeout: TimeInterval { 90 }

    struct Arm {
        let name: String
        let threads: Int
        let qos: QualityOfService
    }

    static let seconds = 2.0, warmUp = 0.3
    static let arms: [Arm] =
        [Arm(name: "base_before", threads: 0, qos: .default)]
            + [1, 2, 4, 8].map { Arm(name: "df\($0)", threads: $0, qos: .default) }
            + [1, 2, 4, 8].map { Arm(name: "ui\($0)", threads: $0, qos: .userInteractive) }
            + [Arm(name: "base_after", threads: 0, qos: .default)]

    func run(_ context: TestContext) async throws -> Outcome {
        let scene = await context.onStage(fullScreen: true) { stage -> HogScene in HogScene(stage) }
        await scene.start()
        var results: [HogArm] = []
        for arm in Self.arms {
            results.append(await scene.measure(arm, warmUp: Self.warmUp, seconds: Self.seconds))
        }
        await scene.stop()
        return Self.verdict(results)
    }

    static func verdict(_ results: [HogArm]) -> Outcome {
        let bases = results.filter { $0.arm.threads == 0 }
        let pm = bases.map(\.present.missed).max() ?? 0, lm = bases.map(\.link.missed).max() ?? 0
        let late = bases.map(\.late).max() ?? 0
        var metrics = Metrics()
        metrics.set("base_present_hist", bases.map(\.present.histogramText).joined(separator: ","))
        var checks: [(String, Bool)] = bases.map {
            ("\($0.arm.name): \($0.present.frames) probe frames shown in \(seconds) s", $0.present.frames >= 100)
        }
        for result in results {
            let name = result.arm.name
            metrics.set(name + "_pm", result.present.missed)
            metrics.set(name + "_lm", result.link.missed)
            metrics.set(name + "_late", result.late)
            guard result.arm.threads > 0 else { continue }
            metrics.set(name + "_cpu", Int((result.leastShare * 100).rounded()))
            checks += [
                (
                    "\(name): the display missed \(result.present.missed) frames, base \(pm)",
                    result.present.missed <= pm + 1
                ),
                (
                    "\(name): the main thread missed \(result.link.missed) frames, base \(lm)",
                    result.link.missed <= lm + 1
                ),
                ("\(name): \(result.late) callbacks began late, base \(late)", result.late <= late + 2),
                ("\(name): every spinner ran (least share \(Int(result.leastShare * 100))%)", result.leastShare >= 0.1),
            ]
        }
        return .judged(checks, metrics)
    }
}

/// One arm's window: the main thread's display-link frames, the probe's shown frames, the late callbacks, and each
/// spinner's CPU time.
struct HogArm {
    let arm: HogPacingTest.Arm
    let link: Pacing
    let present: Pacing
    let lateFrames: LateFrames
    let late: Int
    let lateMaxMs: Double
    let spinnerMs: [Double]
    let windowMs: Double

    var leastShare: Double { windowMs > 0 ? (spinnerMs.min() ?? 0) / windowMs : 0 }
}

/// pacing.text's labels redrawn at every frame of the main thread's display link, and the frame probe in a corner.
@MainActor
final class HogScene: NSObject {
    private let animate: (Int) -> Void
    private let probe: FrameProbe
    private var link: CADisplayLink?
    private var frame = 0
    private var ticks: [(timestamp: Double, lateMs: Double)] = []

    init(_ stage: UIView) {
        let screen = UIScreen.main.bounds
        animate = PacingTests.text.build(stage, screen)
        probe = FrameProbe(frame: CGRect(x: screen.width - 58, y: 10, width: 48, height: 48))
        stage.layer.addSublayer(probe.layer)
        super.init()
    }

    func start() {
        probe.start()
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    func stop() async {
        link?.invalidate()
        link = nil
        await probe.stop()
    }

    @objc private func tick(_ link: CADisplayLink) {
        frame += 1
        ticks.append((link.timestamp, (CACurrentMediaTime() - link.targetTimestamp) * 1000))
        animate(frame)
    }

    /// `arm`'s spinners from now for `warmUp + seconds`, the window the last `seconds` of that.
    func measure(_ arm: HogPacingTest.Arm, warmUp: Double, seconds: Double) async -> HogArm {
        let started = CACurrentMediaTime()
        let from = started + warmUp, to = from + seconds
        let spinners = await Spinners.run(arm.threads, arm.qos, until: to, otherwise: from + seconds)
        let window = ticks.filter { $0.timestamp >= from && $0.timestamp <= to }
        let shown = probe.presented(from: from, to: to)
        let result = HogArm(
            arm: arm, link: Pacing(times: window.map(\.timestamp), period: 1.0 / 60),
            present: Pacing(times: shown.sorted(), period: 1.0 / 60),
            lateFrames: LateFrames(presented: shown, period: 1.0 / 60), late: window.filter { $0.lateMs > 0 }.count,
            lateMaxMs: window.map(\.lateMs).max() ?? 0, spinnerMs: spinners, windowMs: seconds * 1000
        )
        ticks.removeAll { $0.timestamp < to }
        SwitchMarks.mark(
            "cpu.hog_pacing.\(arm.name)", since: from,
            "present=\(result.present.histogramText) unshown=\(shown.filter { $0 <= 0 }.count) "
                + "link=\(result.link.histogramText) late=\(result.late) late_max_ms="
                + String(format: "%.1f", result.lateMaxMs) + " phases=\(result.lateFrames.phases()) "
                + "on_grid=\(result.lateFrames.onGrid) spin_cpu_ms="
                + spinners.map { String(Int($0.rounded())) }.joined(separator: ",")
        )
        return result
    }
}

/// Threads that spin until a deadline of their own (a test that times out leaves none behind), each one's CPU time.
enum Spinners {
    /// What the spinners computed, kept so the optimiser keeps their loops
    private static var sink: UInt64 = 0

    /// `count` spinners at `qos` until `deadline` (CACurrentMediaTime's clock); with none, the wait until `otherwise`.
    static func run(_ count: Int, _ qos: QualityOfService, until deadline: Double,
                    otherwise: Double) async -> [Double] {
        let done = Waiter<[Double]>()
        guard count > 0 else {
            DispatchQueue.main.asyncAfter(deadline: .now() + max(0, otherwise - CACurrentMediaTime())) {
                done.resume([])
            }
            return await done.wait(timeout: otherwise - CACurrentMediaTime() + 5) ?? []
        }
        let lock = NSLock()
        var cpu: [Double] = []
        let group = DispatchGroup()
        let end = deadline
        for _ in 0 ..< count {
            group.enter()
            let thread = Thread {
                let (ms, value) = spin(until: end)
                lock.lock()
                cpu.append(ms)
                sink ^= value
                lock.unlock()
                group.leave()
            }
            thread.qualityOfService = qos
            thread.start()
        }
        group.notify(queue: .main) {
            lock.lock()
            let all = cpu
            lock.unlock()
            done.resume(all)
        }
        return await done.wait(timeout: deadline - CACurrentMediaTime() + 5) ?? []
    }

    /// The calling thread busy until `deadline`: its CPU time in milliseconds, and what it computed.
    static func spin(until deadline: Double) -> (Double, UInt64) {
        let start = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)
        var x: UInt64 = 0x9E37_79B9_7F4A_7C15
        while CACurrentMediaTime() < deadline {
            for _ in 0 ..< 4096 { x = x &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407 }
        }
        return (Double(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - start) / 1e6, x)
    }
}
