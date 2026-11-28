import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import Metal
import QuartzCore
import UIKit

/// Hardware probe comparing MME macro scissor bands with class WAIT_FOR_IDLE (0x0110) and no host
/// semaphore (SWITCH_GM20B_MACRO_BAND_DRAW) against CPU-emitted scissor bands with per-band host
/// fences. Checks pixel identity against an unbanded reference, then runs back-to-back long draws
/// under a spinning layer in both modes and reports `preempt_max_us`, `preempt_mid_job`,
/// `preempt_unknown`, and compositor sync latency.
struct ProbeMacroWfiTest: HWTest {
    let id = "gpu.sched.probe_macro_wfi"
    let name = "Macro WFI band draw probe: compositor wait and pixel identity against CPU bands"
    let subsystem = Subsystem.metal

    static let width = DrawBandsTest.width
    static let height = DrawBandsTest.height
    static let draws = DrawBandsTest.draws
    static let budgetUs = DrawBandsTest.budgetUs
    static let kickSeconds = DrawBandsTest.kickSeconds

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        guard let production = DrawBandsTest.budget(gpu.device, Self.budgetUs * 1000) else {
            return .skip(GM20BFacade.missing("switchMetalBandBudget:", on: gpu.device))
        }
        guard Self.supportsMacroBands(gpu.device) else {
            DrawBandsTest.budget(gpu.device, production)
            return .skip(GM20BFacade.missing("switchMetalProbeMacroBands:", on: gpu.device))
        }
        defer {
            Self.macroBands(gpu.device, false)
            DrawBandsTest.budget(gpu.device, production)
        }

        let pipeline: MTLRenderPipelineState
        do {
            pipeline = try gpu.render(
                vertex: "bands_vertex", fragment: "bands_blend_fragment", format: .bgra8Unorm
            ) { descriptor in
                let target = descriptor.colorAttachments[0]!
                target.isBlendingEnabled = true
                target.sourceRGBBlendFactor = .sourceAlpha
                target.destinationRGBBlendFactor = .oneMinusSourceAlpha
            }
        } catch {
            return .skip("no blend pipeline: \(GPU.describe(error))")
        }

        let target = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        var metrics = Metrics()
        var failures: [String] = []

        _ = DrawBandsTest.budget(gpu.device, 0)
        Self.macroBands(gpu.device, false)
        try await gpu.run { commands in DrawBandsTest.encode(commands, pipeline, target, samples: 1, resolve: nil) }
        let wholeBytes = try await gpu.read(target, bytesPerPixel: 4)

        _ = DrawBandsTest.budget(gpu.device, Self.budgetUs * 1000)
        for (arm, macroOn) in [("_cpu", false), ("_macro", true)] {
            Self.macroBands(gpu.device, macroOn)
            let before = GM20BFacade.counters(of: gpu.device)
            try await gpu.run { commands in DrawBandsTest.encode(commands, pipeline, target, samples: 1, resolve: nil) }
            let after = GM20BFacade.counters(of: gpu.device)
            let bands = (after["bands"] ?? 0) - (before["bands"] ?? 0)
            let bytes = try await gpu.read(target, bytesPerPixel: 4)
            let identical = bytes == wholeBytes
            metrics.set("bands" + arm, bands)
            metrics.set("identical" + arm, identical)
            if !identical { failures.append("macro_wfi\(arm): banded pixels differed from whole draw") }
            if bands < 2 * Self.draws {
                failures.append("macro_wfi\(arm): \(bands) bands for \(Self.draws) draws")
            }
        }

        let layer = await context.onStage { stage -> CALayer in
            let spinner = CALayer()
            spinner.frame = CGRect(x: 20, y: 20, width: 80, height: 80)
            spinner.backgroundColor = UIColor.systemPurple.cgColor
            let spin = CABasicAnimation(keyPath: "transform.rotation.z")
            spin.fromValue = 0
            spin.toValue = 2 * Double.pi
            spin.duration = 1
            spin.repeatCount = .infinity
            spinner.add(spin, forKey: "spin")
            stage.layer.addSublayer(spinner)
            return spinner
        }

        let connection = GPUConnection()
        if case .unreadable("release_kernel", ENOENT) = GPUPaths.read(connection) {
            await MainActor.run { layer.removeFromSuperlayer() }
            await context.clearStage()
            metrics.fitLog(within: 720, keeping: Self.judged)
            if !failures.isEmpty { return .fail(failures.joined(separator: "; "), metrics) }
            let why = KernelSysctl.isReleaseKernel()
                ? "no preemption counters: a release kernel"
                : "no preemption counters (build with --//bazel:stats_gpu)"
            return .skip(why, metrics)
        }
        for (arm, macroOn) in [("_cpu", false), ("_macro", true)] {
            Self.macroBands(gpu.device, macroOn)
            _ = GPUPaths.read(connection)
            let syncBefore = connection.flatMap { LaunchHitchTest.spans($0) }
            var passes = 0
            var totalMs = 0.0
            let until = CFAbsoluteTimeGetCurrent() + Self.kickSeconds
            while CFAbsoluteTimeGetCurrent() < until {
                let timing = try await gpu.run { commands in
                    DrawBandsTest.encode(commands, pipeline, target, samples: 1, resolve: nil)
                }
                passes += 1
                totalMs += timing.gpuMilliseconds
            }
            metrics.set("passes" + arm, passes)
            if passes > 0 {
                metrics.set("us_per_pass" + arm, totalMs * 1000 / Double(passes), digits: 0)
            }
            guard case .counts(let after) = GPUPaths.read(connection) else {
                failures.append("the kernel's paths line is unreadable: no preemption latencies")
                break
            }
            if syncBefore != nil, let line = connection.flatMap({ LaunchHitchTest.spans($0) }) {
                let sync = SpanReading.parse(line)
                for key in ["sync_subs", "behind", "behind_sync_us", "sync_max_us", "windows", "holds"] {
                    metrics.set(key + arm, sync.counts[key] ?? 0)
                }
            }
            for key in DrawBandsTest.preemptKeys { metrics.set(key + arm, after.counts[key] ?? 0) }
            if (after.counts["preempts"] ?? 0) == 0 {
                failures.append("macro_wfi\(arm): no compositor kick met banded draw in \(Self.kickSeconds) s")
            }
        }
        await MainActor.run { layer.removeFromSuperlayer() }
        await context.clearStage()
        metrics.fitLog(within: 720, keeping: Self.judged)
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }

    static let judged: Set<String> = [
        "identical_cpu", "identical_macro", "bands_cpu", "bands_macro", "preempts_cpu", "preempts_macro",
        "preempt_max_us_cpu", "preempt_max_us_macro", "preempt_mid_job_cpu", "preempt_mid_job_macro",
        "preempt_unknown_cpu", "preempt_unknown_macro", "sync_max_us_cpu", "sync_max_us_macro",
        "us_per_pass_cpu", "us_per_pass_macro",
    ]

    static func supportsMacroBands(_ device: MTLDevice) -> Bool {
        let selector = NSSelectorFromString("switchMetalProbeMacroBands:")
        return (device as? NSObject)?.responds(to: selector) == true
    }

    @discardableResult
    static func macroBands(_ device: MTLDevice, _ on: Bool) -> String? {
        let selector = NSSelectorFromString("switchMetalProbeMacroBands:")
        guard let object = device as? NSObject, object.responds(to: selector) else { return nil }
        return object.perform(selector, with: NSNumber(value: on))?.takeUnretainedValue() as? String
    }
}
