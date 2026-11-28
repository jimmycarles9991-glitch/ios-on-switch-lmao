import Foundation
import HWTestCore
import HWTestKit
import QuartzCore
import UIKit

/// The owner's "Spotlight and rotation take about twice as long": a 300 ms move driven five ways (AnimMechanism) and
/// the app's own scene turned by 90 and 180 degrees (AnimTurns), each on an idle stage, on pacing.glass_under's scene
/// (20 to 30 frames a second on the 22 Nov run) and with the main thread blocked 300 ms after the move is added. Each
/// must take its nominal time from its begin to its end (AnimRun) within three periods and the run's worst shown
/// interval, and each in-process move's values must keep within 5% of its curve at the time that passed. The render
/// server's moves are measured on the app's clock (commit stamp to completion); their frames on glass need a readback
/// of the panel. Metrics per run: <scene>_<mechanism>=duration/nominal/start ms[/error]; per scene the late probe
/// frames and their phases on the 250 ms grid of absolute time.
struct AnimTimingTest: HWTest {
    let id = "anim.timing"
    let name = "A 300 ms move takes 300 ms whatever drives it, under load and with the main thread blocked"
    let subsystem = Subsystem.compositor
    var timeout: TimeInterval { 120 }

    static let scenes: [(name: String, loaded: Bool, block: Bool)] = [
        ("idle", false, false), ("loaded", true, false), ("blocked", false, true),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        var runs: [AnimRun] = []
        var metrics = Metrics()
        var restored = true
        let watcher = await MainActor.run { AnimTurns.install() }
        for scene in Self.scenes {
            let stage = await context.onStage(fullScreen: true) { AnimStage($0, loaded: scene.loaded) }
            await MainActor.run { stage.start() }
            let began = CACurrentMediaTime()
            for mechanism in AnimMechanism.allCases {
                runs.append(await AnimRun.run(mechanism, on: stage, scene: scene.name, block: scene.block))
            }
            if let watcher {
                for turn in AnimTurn.allCases {
                    let (run, back, cost) = await AnimTurns.run(
                        turn, watcher: watcher, stage: stage, scene: scene.name, block: scene.block
                    )
                    runs.append(run)
                    if let cost { metrics.set("\(scene.name)_\(turn.rawValue)_cost", cost.metric) }
                    restored = restored && back
                }
            }
            await stage.stop()
            let shown = await MainActor.run { stage.probe.presented(from: began, to: CACurrentMediaTime()) }
            metrics.merge(LateFrames(presented: shown, period: AnimRun.period).metrics(), prefix: scene.name + "_")
            await context.clearStage()
        }
        if let watcher { await MainActor.run { AnimTurns.uninstall(watcher) } }
        return Self.verdict(runs, metrics, turns: watcher != nil, restored: restored)
    }

    static func verdict(_ runs: [AnimRun], _ sceneMetrics: Metrics, turns: Bool, restored: Bool) -> Outcome {
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for run in runs {
            metrics.set("\(run.scene)_\(run.mechanism)", run.metric)
            let note = run.note.isEmpty ? "" : " (\(run.note))"
            checks.append(
                (
                    "\(run.scene) \(run.mechanism): \(Int(run.durationMs)) ms for a nominal \(Int(run.nominal * 1000)) "
                        + "ms, within \(Int(run.toleranceMs))\(note)", run.onTime
                )
            )
            if run.error >= 0 {
                checks.append(
                    (
                        "\(run.scene) \(run.mechanism): values within 5% of the curve at the time passed (worst "
                            + String(format: "%.3f", run.error) + ")", run.error <= 0.05
                    )
                )
            }
        }
        metrics.merge(sceneMetrics)
        metrics.set("turns", turns)
        checks.append(("the root view controller took the turn watcher", turns))
        if turns { checks.append(("the scene turned back after every turn", restored)) }
        return .judged(checks, metrics)
    }
}
