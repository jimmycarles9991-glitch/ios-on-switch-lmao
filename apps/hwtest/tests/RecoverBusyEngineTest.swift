import Foundation
import HWTestCore
import HWTestGPU
import Metal
import QuartzCore
import UIKit

/// T-v3g's poisoned save, on purpose, while the compositor presents every frame. The bench's channel runs the 2D solid
/// fills of batch twod-g7: four floats (RF32x4) into an RGBA16F target (a class error, its channel killed), then into a
/// BGRA8 target on the channel made again, a job that completes and leaves GR unable to save that channel's context.
/// The kernel decides such a save at the first context-switch timeout tick: the bench's channel alone dies, GR is reset,
/// and the compositor's frames wait no longer than the bound. Before x1-kern7 nothing decided it and the present's
/// deadline panicked.
struct RecoverBusyEngineTest: HWTest {
    let id = "gpu.recover.busy_engine"
    let name =
        "A job that leaves GR unable to save its context: only its channel dies, the compositor waits under the bound"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let rounds = 3
    static let gapBoundMs = 600.0
    static let floats: [UInt32] = [1_045_220_557, 1_053_609_165, 1_058_642_330, 1_065_353_216]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        let runner: BenchRunner
        let probeBench: BenchRunner
        let fills: [BenchExperiment]
        do {
            runner = try BenchRunner.open()
            probeBench = try BenchRunner.open()
            fills = try Self.experiments()
        } catch {
            return .fail("the bench did not open: \(error)")
        }
        let compositor = await context.onStage { stage -> RecoverCompositorProbe in
            let probe = RecoverCompositorProbe(device: gpu.device, bench: probeBench)
            probe.probe.frame = CGRect(x: stage.bounds.width - 58, y: 10, width: 48, height: 48)
            probe.probe.contentsScale = UIScreen.main.scale
            stage.layer.addSublayer(probe.probe)
            return probe
        }
        await MainActor.run { compositor.start() }
        try await Task.sleep(nanoseconds: 500_000_000)
        await MainActor.run { compositor.measure() }

        var statuses: [String] = []
        for _ in 0..<Self.rounds {
            for fill in fills {
                let outcome = runner.run(fill)
                statuses.append("\(fill.id):\(outcome.status)")
            }
            // the save that cannot complete comes when the compositor's next kick switches GR away
            try await Task.sleep(nanoseconds: 1_500_000_000)
        }
        await MainActor.run { compositor.stop() }
        let presented = await MainActor.run { compositor.presented }
        let fillErrors = await MainActor.run { compositor.fillErrors }
        await context.clearStage()

        let shown = Pacing(times: presented.sorted(), period: 1.0 / 60)
        let gapMs = shown.intervals.max() ?? 0
        var metrics = Metrics()
        metrics.set("rounds", Self.rounds)
        metrics.set("bench", String(statuses.joined(separator: ",").prefix(200)))
        metrics.set("present_gap_ms", gapMs, digits: 1)
        metrics.set("gap_bound_ms", Self.gapBoundMs, digits: 0)
        metrics.set("present_frames", shown.frames)
        metrics.set("probe_fill_errors", fillErrors)
        return .judged(
            [
                ("every bench run returned", statuses.count == Self.rounds * fills.count),
                ("the compositor kept presenting within the bound", shown.frames > 0 && gapMs <= Self.gapBoundMs),
                ("the probe's own raw device filled every frame", fillErrors == 0),
            ], metrics)
    }

    static func experiments() throws -> [BenchExperiment] {
        let fill = { (id: String, format: String) -> [String: Any] in
            [
                "id": id, "kind": "twod", "readback": "full", "rect": [1, 1, 15, 15],
                "target": ["format": format, "width": 16, "height": 16],
                "twod": ["op": "fill", "colorFormat": 192, "color": floats, "reference": true],
            ]
        }
        let batch: [String: Any] = [
            "protocol": BenchProtocol.version, "batch": "busy-engine",
            "experiments": [fill("fill-f32-rgba16float", "rgba16float"), fill("fill-f32-bgra8unorm", "bgra8unorm")],
        ]
        let data = try JSONSerialization.data(withJSONObject: batch)
        return try BenchBatch.decode(data).entries.compactMap {
            if case .experiment(let e) = $0 { return e }
            return nil
        }
    }
}
