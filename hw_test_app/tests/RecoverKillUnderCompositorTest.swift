import Foundation
import HWTestCore
import HWTestGPU
import Metal
import QuartzCore
import UIKit

/// gpu.recover.kill_one's kill again and again while the compositor presents every frame (a probe layer a bench raw
/// device fills, RecoverCompositorProbe). Each GR error resets GR, and
/// with the compositor drawing every frame the PBDMA is often on its channel when that reset comes: a reset that leaves
/// the compositor's context load stuck shows as a frame waiting a context-switch timeout tick or more. Each kill fails
/// only its own command buffer, the raw channel comes back each time, and no frame waits past the bound.
struct RecoverKillUnderCompositorTest: HWTest {
    let id = "gpu.recover.kill_under_compositor"
    let name = "Raw channel kills while the compositor presents every frame: each fails only itself, no frame waits"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let rounds = 16
    static let gapBoundMs = 100.0

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        let start = RawFacade.counters(of: gpu.device)
        guard start["gpu_order"] != nil else { return .skip("the facade has no recovery counters (a release build)") }
        let probeBench: BenchRunner
        do {
            probeBench = try BenchRunner.open()
        } catch {
            return .fail("the compositor probe's bench raw device did not open: \(error)")
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

        let kill = RecoverKillOneTest()
        let ring = try gpu.buffer(2 * Self.rounds * RecoverKillOneTest.slice)
        let scratch = try gpu.texture(.bgra8Unorm, 16, 16, usage: [.shaderRead])
        let ends = CompletionTimes()
        var fills: [MTLCommandBuffer] = []
        var poisoned: [MTLCommandBuffer] = []
        var refusal: String?
        var allEnded = true
        for round in 0..<Self.rounds {
            fills.append(try kill.fill(gpu, ring, slot: 2 * round, ends))
            let (bad, refused) = try kill.poison(gpu, ring, slot: 2 * round + 1, scratch, ends)
            refusal = refusal ?? refused
            poisoned.append(bad)
            allEnded = await ends.wait(fills + poisoned, seconds: 10) && allEnded
            // a different point of the compositor's frame for each kill
            try await Task.sleep(nanoseconds: UInt64.random(in: 3_000_000...29_000_000))
        }
        let last = try kill.fill(gpu, ring, slot: 2 * Self.rounds - 1, ends)
        allEnded = await ends.wait([last], seconds: 10) && allEnded
        try await Task.sleep(nanoseconds: 300_000_000)
        await MainActor.run { compositor.stop() }
        let presented = await MainActor.run { compositor.presented }
        let (fillErrors, firstFillError) = await MainActor.run { (compositor.fillErrors, compositor.firstFillError) }
        await context.clearStage()
        let end = RawFacade.counters(of: gpu.device)

        let bytes = UnsafeRawBufferPointer(start: ring.contents(), count: ring.length)
        let slice = RecoverKillOneTest.slice
        let kept = (0..<Self.rounds).filter { round in
            bytes[(2 * round * slice)..<((2 * round + 1) * slice)].allSatisfy { $0 == UInt8(2 * round + 1) }
        }.count
        let fillsOK = (fills + [last]).filter { $0.status == .completed && $0.error == nil }.count
        let killed = poisoned.filter { $0.status == .error && RawFacade.path(of: $0) == "raw" }.count
        let delta = { (key: String) in (end[key] ?? 0) - (start[key] ?? 0) }
        let shown = Pacing(times: presented.sorted(), period: 1.0 / 60)
        let presentGapMs = shown.intervals.max() ?? 0

        var metrics = Metrics()
        metrics.set("rounds", Self.rounds)
        metrics.set("killed", killed)
        metrics.set("fills_ok", "\(fillsOK)/\(Self.rounds + 1)")
        metrics.set("fill_bytes", kept)
        metrics.set("reopens", delta("reopens"))
        metrics.set("cpu_ordered", delta("cpu_ordered"))
        metrics.set("present_gap_ms", presentGapMs, digits: 1)
        metrics.set("gap_bound_ms", Self.gapBoundMs, digits: 0)
        metrics.set("present_frames", shown.frames)
        metrics.set("probe_fill_errors", fillErrors)
        if !firstFillError.isEmpty { metrics.set("probe_fill_first", String(firstFillError.prefix(80))) }
        if let refusal { return .fail("the illegal method was refused: \(refusal)", metrics) }
        return .judged(
            [
                ("every command buffer ended (none hung)", allEnded),
                ("every poisoned command buffer ran raw and failed", killed == Self.rounds),
                ("every fill completed with its bytes", fillsOK == Self.rounds + 1 && kept == Self.rounds),
                ("each kill made one reopen", delta("reopens") == Self.rounds),
                ("no CPU-ordered fallback; GPU order on", delta("cpu_ordered") == 0 && end["gpu_order"] == 1),
                (
                    "the compositor kept presenting within the bound",
                    shown.frames > 0 && presentGapMs <= Self.gapBoundMs
                ),
                ("the probe's own raw device filled every frame", fillErrors == 0),
            ], metrics)
    }
}
