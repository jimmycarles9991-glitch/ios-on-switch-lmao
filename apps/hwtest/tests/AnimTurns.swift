import Foundation
import HWTestCore
import HWTestKit
import QuartzCore
import UIKit

/// The start of a turn of the app's scene: UIKit's viewWillTransition with the coordinator's transitionDuration, or
/// the scene's refusal of the request.
enum TurnStart {
    case began(at: Double, nominal: Double)
    case refused(String)
}

/// A child of the root view controller that hands the next transition's start and its coordinator's completion to
/// the waiters it was given (CACurrentMediaTime's clock).
@MainActor
final class TurnWatcher: UIViewController {
    private var began: Waiter<TurnStart>?
    private var ended: Waiter<Double>?

    func expect(_ began: Waiter<TurnStart>, _ ended: Waiter<Double>) {
        self.began = began
        self.ended = ended
    }

    override func viewWillTransition(to size: CGSize, with coordinator: UIViewControllerTransitionCoordinator) {
        super.viewWillTransition(to: size, with: coordinator)
        let began = began, ended = ended
        self.began = nil
        self.ended = nil
        began?.resume(.began(at: CACurrentMediaTime(), nominal: coordinator.transitionDuration))
        _ = coordinator.animate(alongsideTransition: nil) { _ in ended?.resume(CACurrentMediaTime()) }
    }
}

/// The turns anim.timing makes: a quarter (landscape to portrait and the other way) and a half.
enum AnimTurn: String, CaseIterable {
    case rotate90, rotate180

    func target(_ from: UIInterfaceOrientationMask) -> UIInterfaceOrientationMask {
        let portrait = from == .portrait || from == .portraitUpsideDown
        switch self {
        case .rotate90: return portrait ? .landscapeRight : .portrait
        case .rotate180:
            if portrait { return from == .portrait ? .portraitUpsideDown : .portrait }
            return from == .landscapeLeft ? .landscapeRight : .landscapeLeft
        }
    }
}

/// anim.timing's turns of the app's own scene by 90 and 180 degrees: the render server animates them over the
/// coordinator's transitionDuration, the nominal; viewWillTransition to the coordinator's completion is the measure.
/// Every turn is followed by one back to where the scene was.
@MainActor
enum AnimTurns {
    static func install() -> TurnWatcher? {
        guard let root = scene?.keyWindow?.rootViewController else { return nil }
        let watcher = TurnWatcher()
        root.addChild(watcher)
        watcher.view.frame = .zero
        watcher.view.isUserInteractionEnabled = false
        root.view.addSubview(watcher.view)
        watcher.didMove(toParent: root)
        return watcher
    }

    static func uninstall(_ watcher: TurnWatcher) {
        watcher.willMove(toParent: nil)
        watcher.view.removeFromSuperview()
        watcher.removeFromParent()
    }

    static var scene: UIWindowScene? {
        UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
    }

    static func mask(_ orientation: UIInterfaceOrientation) -> UIInterfaceOrientationMask {
        switch orientation {
        case .portrait: return .portrait
        case .portraitUpsideDown: return .portraitUpsideDown
        case .landscapeLeft: return .landscapeLeft
        default: return .landscapeRight
        }
    }

    /// `turn` from where the scene is, and back; the run, whether the scene was back where it began afterwards, and
    /// what the outward turn cost this app (TurnCost).
    static func run(
        _ turn: AnimTurn, watcher: TurnWatcher, stage: AnimStage, scene sceneName: String, block: Bool
    ) async -> (AnimRun, Bool, TurnCost?) {
        let name = turn.rawValue
        guard let scene = Self.scene else {
            return (refused(name, sceneName, CACurrentMediaTime(), "no_window_scene"), false, nil)
        }
        let from = mask(scene.interfaceOrientation)
        await AnimRun.frames(2)
        let t0 = CACurrentMediaTime()
        let main = await MainActor.run { pthread_mach_thread_np(pthread_self()) }
        let ((start, end), cost) = await TurnCost.around(main: main) {
            await self.turn(to: turn.target(from), watcher: watcher, block: block)
        }
        await AnimRun.frames(8)
        if mask(scene.interfaceOrientation) != from {
            _ = await self.turn(to: from, watcher: watcher, block: false)
            await AnimRun.frames(8)
        }
        let restored = mask(scene.interfaceOrientation) == from
        guard case .began(let at, let nominal)? = start else {
            let note: String
            if case .refused(let why)? = start { note = "refused_" + why } else { note = "no_transition" }
            return (refused(name, sceneName, t0, note), restored, cost)
        }
        let run = AnimRun(
            name, sceneName, t0: t0, begin: at, end: end, nominal: nominal, error: -1, values: 0,
            shown: stage.probe.presented(from: t0, to: max(end, t0)),
            note: end > 0 ? "" : "no_completion"
        )
        return (run, restored, cost)
    }

    private static func turn(
        to mask: UIInterfaceOrientationMask, watcher: TurnWatcher, block: Bool
    ) async -> (TurnStart?, Double) {
        let began = Waiter<TurnStart>(), ended = Waiter<Double>()
        watcher.expect(began, ended)
        scene?.keyWindow?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
        scene?.requestGeometryUpdate(.iOS(interfaceOrientations: mask)) { error in
            began.resume(.refused(LogLine.sanitized(error.localizedDescription)))
        }
        if block {
            let until = CACurrentMediaTime() + AnimRun.blockSeconds
            while CACurrentMediaTime() < until {}
        }
        let start = await began.wait(timeout: AnimRun.blockSeconds + 3)
        guard case .began(_, let nominal)? = start else { return (start, 0) }
        return (start, await ended.wait(timeout: nominal + 3) ?? 0)
    }

    private static func refused(_ name: String, _ scene: String, _ t0: Double, _ note: String) -> AnimRun {
        AnimRun(name, scene, t0: t0, begin: 0, end: 0, nominal: 0, error: -1, values: 0, shown: [], note: note)
    }
}
