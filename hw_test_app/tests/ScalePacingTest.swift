import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import Metal
import QuartzCore
import UIKit

/// Scale animations as an app's open and close run them, judged by what reached the panel and what the compositor did:
/// a full-screen grouped layer (its subviews composited as one, as an app's snapshot is) scaled 0.3 -> 1.0 while it
/// fades in, the reverse, and a UIKit zoom transition (iOS 18 and later; iOS 17 runs the open twice), five rounds each.
/// A probe CAMetalLayer presents every display-link frame and collects when the display showed it. Around each round the
/// debug kernel answers the compositor's round (SwitchGpuCompositorRound.h through the spans op): its jobs and GPU time,
/// jobs longer than a refresh period, surfaces it imported, BoAllocs it made on its own thread (no zeroed spare), and the
/// passes of its push-scanned submissions. A round fails on a missed display period, an import, a cold BoAlloc, a
/// compositor job over one period, fewer than half its frames shown by the probe, or a compositor that did not animate.
struct ScalePacingTest: HWTest {
    let id = "ui.scale_pacing"
    let name = "Scale animations hold every display period with no import and no compositor job over a period"
    let subsystem = Subsystem.compositor
    var timeout: TimeInterval { 90 }

    static let rounds = 5
    static let seconds = 0.5
    static let period = 1.0 / 60
    static let targetPeaksOp: UInt32 = 1

    enum Kind: String, CaseIterable {
        case open, close, zoom
    }

    struct Round {
        var missed = 0, shown = 0, imports = 0, importUs = 0, coldAllocs = 0, overPeriod = 0, jobs = 0, gpuUs = 0,
            scanned = 0, passes = 0, jobMaxUs = 0
    }

    func run(_ context: TestContext) async throws -> Outcome {
        guard let connection = GPUConnection() else { return .skip("no TegraGM20B connection") }
        guard LaunchHitchTest.spans(connection) != nil else {
            return .skip("the kernel keeps no compositor rounds (a release kernel)")
        }
        let probe = await context.onStage(fullScreen: true) { stage -> ScaleProbe in
            let probe = ScaleProbe(stage: stage)
            probe.start()
            return probe
        }
        try await Task.sleep(nanoseconds: 500_000_000)
        var metrics = Metrics()
        var failures: [String] = []
        for kind in Kind.allCases {
            for i in 0..<Self.rounds {
                _ = LaunchHitchTest.spans(connection)
                let from = CACurrentMediaTime()
                await probe.animate(kind, seconds: Self.seconds)
                try await Task.sleep(nanoseconds: 150_000_000)
                let to = CACurrentMediaTime()
                let reading = SpanReading.parse(LaunchHitchTest.spans(connection) ?? "")
                var r = Round()
                (r.missed, r.shown) = await probe.missed(from: from, to: to, period: Self.period)
                r.coldAllocs = reading.counts["cold_allocs"] ?? 0
                r.imports = reading.counts["imports"] ?? 0
                r.importUs = reading.counts["import_us"] ?? 0
                r.overPeriod = reading.counts["over_period"] ?? 0
                r.jobs = reading.counts["jobs"] ?? 0
                r.gpuUs = reading.counts["gpu_us"] ?? 0
                r.scanned = reading.counts["scanned"] ?? 0
                r.passes = reading.counts["passes"] ?? 0
                r.jobMaxUs = reading.jobMaxUs
                let key = "\(kind.rawValue)\(i)"
                metrics.set("\(key)_missed", r.missed)
                metrics.set("\(key)_shown", r.shown)
                metrics.set("\(key)_cold_allocs", r.coldAllocs)
                metrics.set("\(key)_imports", r.imports)
                metrics.set("\(key)_import_us", r.importUs)
                metrics.set("\(key)_over_period", r.overPeriod)
                metrics.set("\(key)_passes", r.scanned > 0 ? Double(r.passes) / Double(r.scanned) : 0, digits: 1)
                metrics.set("\(key)_gpu_ms", r.jobs > 0 ? Double(r.gpuUs) / Double(r.jobs) / 1000 : 0, digits: 2)
                metrics.set("\(key)_job_max_ms", Double(r.jobMaxUs) / 1000, digits: 2)
                if r.missed > 0 { failures.append("\(key) missed \(r.missed) periods") }
                if r.imports > 0 { failures.append("\(key) imported \(r.imports) surfaces (\(r.importUs) us)") }
                if r.overPeriod > 0 { failures.append("\(key) had \(r.overPeriod) compositor jobs over a period") }
                if r.coldAllocs > 0 { failures.append("\(key) made \(r.coldAllocs) compositor BoAllocs with no spare") }
                let frames = Int((to - from) / Self.period)
                if r.shown < frames / 2 { failures.append("\(key) showed \(r.shown) probe frames of \(frames)") }
                if r.jobs < Int(Self.seconds * 60) / 2 {
                    failures.append("\(key) ran \(r.jobs) compositor jobs: the animation did not run")
                }
            }
        }
        await probe.stop()
        let (kr, reply) = connection.call(
            GPUPaths.method, GPUConnection.words([Self.targetPeaksOp, 0]), outputSize: 512)
        if kr == KERN_SUCCESS,
            let compositor = TargetPeak.parse(String(decoding: reply.prefix { $0 != 0 }, as: UTF8.self))
                .first(where: { $0.owner == "backboardd" })
        {
            metrics.set("compositor_largest_target", "\(compositor.width)x\(compositor.height)x\(compositor.samples)")
        }
        return failures.isEmpty ? .pass(metrics) : .fail(failures.prefix(6).joined(separator: "; "), metrics)
    }
}

/// The stage's grouped layer and zoom transition, and a probe CAMetalLayer whose every frame's presented time is kept.
@MainActor
final class ScaleProbe: NSObject {
    private let stage: UIView
    private let probe = CAMetalLayer()
    private let queue: MTLCommandQueue?
    private let shown = ShownTimes()
    private var link: CADisplayLink?
    private var frame = 0
    private let card: UIView

    init(stage: UIView) {
        self.stage = stage
        let device = MTLCreateSystemDefaultDevice()
        queue = device?.makeCommandQueue()
        probe.device = device
        probe.pixelFormat = .bgra8Unorm
        probe.framebufferOnly = true
        let bounds = UIScreen.main.bounds
        card = ScaleProbe.appLike(frame: bounds)
        super.init()
        stage.addSubview(card)
        probe.frame = CGRect(x: bounds.width - 58, y: 10, width: 48, height: 48)
        probe.contentsScale = UIScreen.main.scale
        stage.layer.addSublayer(probe)
    }

    /// A full-screen view of many layers composited as one group, as an app's snapshot or live scene is in its zoom.
    static func appLike(frame: CGRect) -> UIView {
        let view = UIView(frame: frame)
        view.backgroundColor = .systemBackground
        view.layer.allowsGroupOpacity = true
        view.layer.cornerRadius = 24
        view.clipsToBounds = true
        for row in 0..<8 {
            for column in 0..<6 {
                let cell = UIView(
                    frame: CGRect(
                        x: 20 + CGFloat(column) * frame.width / 6, y: 20 + CGFloat(row) * frame.height / 8,
                        width: frame.width / 6 - 30, height: frame.height / 8 - 20))
                cell.backgroundColor = UIColor(
                    hue: CGFloat(row * 6 + column) / 48, saturation: 0.6, brightness: 0.9, alpha: 1)
                cell.layer.cornerRadius = 12
                view.addSubview(cell)
            }
        }
        return view
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
        card.removeFromSuperview()
        probe.removeFromSuperlayer()
    }

    /// Runs one round's animation and returns once it has ended
    func animate(_ kind: ScalePacingTest.Kind, seconds: Double) async {
        switch kind {
        case .open:
            await scale(from: 0.3, to: 1, alpha: (0, 1), seconds: seconds)
        case .close:
            await scale(from: 1, to: 0.3, alpha: (1, 0), seconds: seconds)
        case .zoom:
            await zoom(seconds: seconds)
        }
    }

    private func scale(from: CGFloat, to: CGFloat, alpha: (CGFloat, CGFloat), seconds: Double) async {
        card.transform = CGAffineTransform(scaleX: from, y: from)
        card.alpha = alpha.0
        await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
            UIView.animate(withDuration: seconds, delay: 0, options: [.curveEaseInOut]) {
                self.card.transform = CGAffineTransform(scaleX: to, y: to)
                self.card.alpha = alpha.1
            } completion: { _ in
                self.card.transform = .identity
                self.card.alpha = 1
                done.resume()
            }
        }
    }

    /// UIKit's zoom transition in and out of a full-screen controller, each awaited to its end
    private func zoom(seconds: Double) async {
        guard let host = stage.window?.rootViewController, #available(iOS 18.0, *) else {
            await scale(from: 0.3, to: 1, alpha: (0, 1), seconds: seconds)
            return
        }
        let presented = UIViewController()
        presented.view = ScaleProbe.appLike(frame: UIScreen.main.bounds)
        presented.modalPresentationStyle = .fullScreen
        let source = card
        presented.preferredTransition = .zoom(sourceViewProvider: { _ in source })
        await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
            host.present(presented, animated: true) { done.resume() }
        }
        await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
            presented.dismiss(animated: true) { done.resume() }
        }
    }

    /// Display periods the probe's presents skipped between `from` and `to` (CACurrentMediaTime's clock), counting from
    /// `from` to the first one shown, and how many it showed
    func missed(from: Double, to: Double, period: Double) -> (missed: Int, shown: Int) {
        let shownTimes = shown.measured.filter { $0 > from && $0 < to }.sorted()
        let times = [from] + shownTimes
        var missed = 0
        for (a, b) in zip(times, times.dropFirst()) {
            let periods = Int(((b - a) / period).rounded())
            missed += max(0, periods - 1)
        }
        return (missed, shownTimes.count)
    }

    @objc private func tick(_: CADisplayLink) {
        frame += 1
        guard let queue, let drawable = probe.nextDrawable(), let commands = queue.makeCommandBuffer() else { return }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = drawable.texture
        pass.colorAttachments[0].loadAction = .clear
        let level = Double(frame % 60) / 60
        pass.colorAttachments[0].clearColor = MTLClearColor(red: level, green: 0.3, blue: 1 - level, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        commands.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
        let shown = shown
        let submitted = CACurrentMediaTime()
        drawable.addPresentedHandler { shown.add($0.presentedTime, submitted: submitted, measured: true) }
        commands.present(drawable)
        commands.commit()
    }
}
