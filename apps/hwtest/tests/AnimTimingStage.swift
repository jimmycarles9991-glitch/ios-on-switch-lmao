import Foundation
import HWTestCommon
import HWTestCore
import HWTestKit
import HWTestShared
import QuartzCore
import SwiftUI
import UIKit

/// The progress values an animation's own code set, with when (CACurrentMediaTime's clock): written on the main thread
/// by SwiftUI's geometry effect or the display-link stepper, read by the test.
final class AnimSamples: @unchecked Sendable {
    private let lock = NSLock()
    private var samples: [AnimSample] = []
    private var reach: (at: Double, done: Waiter<Double>)?

    func add(_ progress: Double) {
        let now = CACurrentMediaTime()
        lock.lock()
        samples.append(AnimSample(time: now, progress: progress))
        let reached = reach.flatMap { progress >= $0.at ? $0.done : nil }
        if reached != nil { reach = nil }
        lock.unlock()
        reached?.resume(now)
    }

    /// `done` resumed with the time of the first value from now on at `progress` or past it.
    func notify(at progress: Double, _ done: Waiter<Double>) {
        lock.lock()
        reach = (progress, done)
        lock.unlock()
    }

    /// The values set at or after `time`.
    func since(_ time: Double) -> [AnimSample] {
        lock.lock()
        defer { lock.unlock() }
        return samples.filter { $0.time >= time }
    }
}

/// A move SwiftUI evaluates in this process at every frame: the geometry effect's progress is its animatable data.
struct AnimProgressEffect: GeometryEffect {
    var progress: Double
    let samples: AnimSamples
    let distance: CGFloat

    var animatableData: Double {
        get { progress }
        set { progress = newValue }
    }

    func effectValue(size _: CGSize) -> ProjectionTransform {
        samples.add(progress)
        return ProjectionTransform(CGAffineTransform(translationX: CGFloat(progress) * distance, y: 0))
    }
}

final class AnimModel: ObservableObject {
    @Published var target = 0.0
}

struct AnimBar: View {
    @ObservedObject var model: AnimModel
    let samples: AnimSamples
    let distance: CGFloat

    var body: some View {
        Rectangle().fill(Color.orange).frame(width: 40, height: 40)
            .modifier(AnimProgressEffect(progress: model.target, samples: samples, distance: distance))
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}

/// anim.timing's stage: the scene (idle, or pacing.glass_under's backdrop moving under glass, a blur before iOS 26),
/// the bar each mechanism moves, and the frame probe, in the corner that stays on the panel when the scene turns.
@MainActor
final class AnimStage: NSObject {
    static let distance: CGFloat = 300

    let view: UIView
    let probe: FrameProbe
    let bar = UIView()
    let layerBar = CALayer()
    let model = AnimModel()
    let swiftUISamples = AnimSamples()
    private let host: UIHostingController<AnimBar>
    private var sceneLink: CADisplayLink?
    private var stripes: UIView?
    private var frame = 0

    init(_ stage: UIView, loaded: Bool) {
        view = stage
        let screen = UIScreen.main.bounds
        if loaded {
            let stripes = BackdropStripes(frame: screen.insetBy(dx: -64, dy: 0))
            stage.addSubview(stripes)
            let effect: UIVisualEffect
            if #available(iOS 26.0, *) {
                effect = UIGlassEffect(style: .regular)
            } else {
                effect = UIBlurEffect(style: .regular)
            }
            let glass = UIVisualEffectView(effect: effect)
            glass.frame = screen.insetBy(dx: 40, dy: 40)
            stage.addSubview(glass)
            self.stripes = stripes
        }
        bar.frame = CGRect(x: 60, y: 120, width: 40, height: 40)
        bar.backgroundColor = .systemYellow
        stage.addSubview(bar)
        layerBar.frame = CGRect(x: 60, y: 180, width: 40, height: 40)
        layerBar.backgroundColor = UIColor.systemTeal.cgColor
        stage.layer.addSublayer(layerBar)
        host = UIHostingController(
            rootView: AnimBar(model: model, samples: swiftUISamples, distance: Self.distance)
        )
        host.view.backgroundColor = .clear
        host.view.frame = CGRect(x: 60, y: 240, width: Self.distance + 60, height: 60)
        stage.addSubview(host.view)
        probe = FrameProbe(frame: CGRect(x: 10, y: 10, width: 48, height: 48))
        stage.layer.addSublayer(probe.layer)
        super.init()
    }

    func start() {
        probe.start()
        guard stripes != nil else { return }
        let link = CADisplayLink(target: self, selector: #selector(moveBackdrop))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
        link.add(to: .main, forMode: .common)
        sceneLink = link
    }

    func stop() async {
        sceneLink?.invalidate()
        sceneLink = nil
        await probe.stop()
    }

    @objc private func moveBackdrop() {
        frame += 1
        stripes?.transform = CGAffineTransform(translationX: CGFloat(frame % 32) * 2 - 32, y: 0)
    }

    /// The begin time Core Animation stamped at commit on the render server's move, on CACurrentMediaTime's clock
    /// (0: none stamped)
    func beginTime(_ mechanism: AnimMechanism) -> Double {
        let layer = mechanism == .ca ? layerBar : bar.layer
        let begins = (layer.animationKeys() ?? []).compactMap { layer.animation(forKey: $0)?.beginTime }
        guard let begin = begins.max(), begin > 0 else { return 0 }
        return layer.convertTime(begin, to: nil)
    }

    /// Every bar back at its start, with no animation.
    func reset() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layerBar.removeAllAnimations()
        layerBar.transform = CATransform3DIdentity
        bar.layer.removeAllAnimations()
        bar.transform = .identity
        CATransaction.commit()
        var transaction = Transaction()
        transaction.disablesAnimations = true
        withTransaction(transaction) { model.target = 0 }
    }
}
