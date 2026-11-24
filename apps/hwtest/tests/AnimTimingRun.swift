import Foundation
import HWTestCore
import QuartzCore
import SwiftUI
import UIKit

/// How anim.timing's 300 ms move is driven. The in-process ones set each frame's value in this process, which the test
/// reads; the render server evaluates ca and uiview, whose values the app cannot read back without the glass.
enum AnimMechanism: String, CaseIterable {
    case ca, uiview, spring, swiftui, link

    var inProcess: Bool { self == .spring || self == .swiftui || self == .link }
    var curve: AnimCurve { self == .spring ? .spring : .linear }
}

/// One move or turn of anim.timing, begun by the test at t0 (the blocked scene then holds the main thread for
/// 300 ms). Its begin and end: for an in-process move the shown time of the probe frame committed after its first
/// value off the start and after its first at 99%; for the render server's, the begin time Core Animation stamped on
/// the animation at commit and its completion; for a turn, viewWillTransition and the coordinator's completion. The
/// nominal is the time its curve takes between those values (the coordinator's transitionDuration for a turn).
struct AnimRun {
    let mechanism: String
    let scene: String
    let t0: Double
    let begin: Double
    let end: Double
    let nominal: Double
    /// AnimProgress.worstError of the in-process values; -1 for the others
    let error: Double
    let values: Int
    let late: LateFrames
    let worstInterval: Double
    let note: String

    static let duration = 0.3, period = 1.0 / 60, blockSeconds = 0.3

    var seen: Bool { begin > 0 && end > begin }
    var startMs: Double { begin > 0 ? (begin - t0) * 1000 : -1 }
    var durationMs: Double { seen ? (end - begin) * 1000 : -1 }
    /// Three periods, and the run's worst shown interval: each end is seen only at a frame the compositor showed.
    var toleranceMs: Double { (3 * Self.period + worstInterval) * 1000 }
    var onTime: Bool { seen && abs(durationMs - nominal * 1000) <= toleranceMs }

    var metric: String {
        "\(Int(durationMs))/\(Int(nominal * 1000))/\(Int(startMs))" + (error >= 0 ? String(format: "/%.3f", error) : "")
    }

    init(
        _ mechanism: String, _ scene: String, t0: Double, begin: Double, end: Double, nominal: Double, error: Double,
        values: Int, shown: [Double], note: String
    ) {
        self.mechanism = mechanism
        self.scene = scene
        self.t0 = t0
        self.begin = begin
        self.end = end
        self.nominal = nominal
        self.error = error
        self.values = values
        late = LateFrames(presented: shown, period: Self.period)
        let times = shown.filter { $0 > 0 }.sorted()
        worstInterval = zip(times, times.dropFirst()).map { $1 - $0 }.max() ?? 0
        self.note = note
        SwitchMarks.mark(
            "anim.timing.\(scene).\(mechanism)", since: t0,
            "duration_ms=\(Int(durationMs)) nominal_ms=\(Int(nominal * 1000)) start_ms=\(Int(startMs)) "
                + "tolerance_ms=\(Int(toleranceMs)) error=" + String(format: "%.3f", error) + " values=\(values) "
                + "late=\(late.late.count) phases=\(late.phases())" + (note.isEmpty ? "" : " note=\(note)")
        )
    }

    @MainActor
    static func run(_ mechanism: AnimMechanism, on stage: AnimStage, scene: String, block: Bool) async -> AnimRun {
        stage.reset()
        await frames(2)
        let done = Waiter<Double>()
        let stepped = AnimSamples()
        var stepper: AnimStepper?
        let t0 = CACurrentMediaTime()
        switch mechanism {
        case .ca:
            let move = CABasicAnimation(keyPath: "transform.translation.x")
            move.fromValue = 0
            move.toValue = AnimStage.distance
            move.duration = duration
            move.timingFunction = CAMediaTimingFunction(name: .linear)
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            CATransaction.setCompletionBlock { done.resume(CACurrentMediaTime()) }
            stage.layerBar.transform = CATransform3DMakeTranslation(AnimStage.distance, 0, 0)
            stage.layerBar.add(move, forKey: "move")
            CATransaction.commit()
        case .uiview:
            UIView.animate(withDuration: duration, delay: 0, options: [.curveLinear]) {
                stage.bar.transform = CGAffineTransform(translationX: AnimStage.distance, y: 0)
            } completion: { _ in
                done.resume(CACurrentMediaTime())
            }
        case .spring, .swiftui:
            stage.swiftUISamples.notify(at: 0.99, done)
            let animation: Animation =
                mechanism == .spring ? .spring(duration: duration, bounce: 0) : .linear(duration: duration)
            withAnimation(animation) { stage.model.target = 1 }
        case .link:
            stepped.notify(at: 0.99, done)
            stepper = AnimStepper(duration: duration, bar: stage.bar, samples: stepped)
            stepper?.start()
        }
        CATransaction.flush()
        let stamped = mechanism.inProcess ? 0 : stage.beginTime(mechanism)
        if block {
            let until = CACurrentMediaTime() + blockSeconds
            while CACurrentMediaTime() < until {}
        }
        let finished = await done.wait(timeout: duration + blockSeconds + 2) ?? 0
        stepper?.stop()
        await frames(8)
        let shown = stage.probe.presented(from: t0, to: max(finished, t0))
        guard mechanism.inProcess else {
            return AnimRun(
                mechanism.rawValue, scene, t0: t0, begin: stamped, end: finished, nominal: duration, error: -1,
                values: 0, shown: shown, note: stamped > 0 ? "" : "no_begin_time"
            )
        }
        let samples = AnimProgress.moving(mechanism == .link ? stepped.since(t0) : stage.swiftUISamples.since(t0))
        let last = samples.first { $0.progress >= 0.99 }
        let first = samples.first
        return AnimRun(
            mechanism.rawValue, scene, t0: t0, begin: first.map { stage.probe.shown(after: $0.time) } ?? 0,
            end: last.map { stage.probe.shown(after: $0.time) } ?? 0,
            nominal: AnimProgress.nominal(
                mechanism.curve, from: first?.progress ?? 0, to: last?.progress ?? 1, duration: duration
            ),
            error: AnimProgress.worstError(samples, curve: mechanism.curve, duration: duration, slack: period),
            values: samples.count, shown: shown, note: last == nil ? "never_reached_99" : ""
        )
    }

    /// The next `count` display-link frames of the main thread, one wait.
    @MainActor
    static func frames(_ count: Int) async {
        let done = Waiter<Bool>()
        let counter = LinkFrameCounter(count, done)
        counter.start()
        _ = await done.wait(timeout: Double(count) / 10 + 1)
        counter.stop()
    }
}

/// The display-link stepper: from its first frame, each frame's value is (targetTimestamp - start) / duration.
@MainActor
final class AnimStepper: NSObject {
    private let duration: Double
    private let bar: UIView
    private let samples: AnimSamples
    private var link: CADisplayLink?
    private var began: Double?

    init(duration: Double, bar: UIView, samples: AnimSamples) {
        self.duration = duration
        self.bar = bar
        self.samples = samples
    }

    func start() {
        let link = CADisplayLink(target: self, selector: #selector(step))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    func stop() {
        link?.invalidate()
        link = nil
    }

    @objc private func step(_ link: CADisplayLink) {
        let start = began ?? link.timestamp
        began = start
        let progress = min(max((link.targetTimestamp - start) / duration, 0), 1)
        bar.transform = CGAffineTransform(translationX: CGFloat(progress) * AnimStage.distance, y: 0)
        samples.add(progress)
        if progress >= 1 { stop() }
    }
}

/// Counts display-link frames on the main thread and resumes its waiter at the last.
@MainActor
final class LinkFrameCounter: NSObject {
    private var left: Int
    private let done: Waiter<Bool>
    private var link: CADisplayLink?

    init(_ count: Int, _ done: Waiter<Bool>) {
        left = count
        self.done = done
    }

    func start() {
        let link = CADisplayLink(target: self, selector: #selector(tick))
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    func stop() {
        link?.invalidate()
        link = nil
    }

    @objc private func tick() {
        left -= 1
        guard left <= 0 else { return }
        stop()
        done.resume(true)
    }
}
