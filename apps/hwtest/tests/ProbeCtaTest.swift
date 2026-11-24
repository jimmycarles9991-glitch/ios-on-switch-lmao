import Foundation
import HWTestCore
import HWTestGPU
import Metal
import QuartzCore
import UIKit

/// Hardware probe of GM20B compute CTA preemption (ctxsw main image option 0x60,
/// TegraGM20BGrPreemptABI.h). Runs a whole long compute dispatch (band budget 0 so CPU grid
/// piecing is off) back to back under a spinning layer, first with WFI (0) and then with CTA (1),
/// reporting `preempt_max_us`, compositor sync latency, option 0x60 readback, and output identity.
struct ProbeCtaTest: HWTest {
    let id = "gpu.sched.probe_cta"
    let name = "Compute CTA preemption probe: compositor wait with and without ctxsw option 0x60"
    let subsystem = Subsystem.metal

    static let threads = DispatchPiecesTest.threads
    static let groupSize = DispatchPiecesTest.groupSize
    static let steps: UInt32 = DispatchPiecesTest.steps
    static let kickSeconds = DrawBandsTest.kickSeconds

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        guard let production = DrawBandsTest.budget(gpu.device, 0) else {
            return .skip(GM20BFacade.missing("switchMetalBandBudget:", on: gpu.device))
        }
        guard Self.supportsComputeCta(gpu.device) else {
            DrawBandsTest.budget(gpu.device, production)
            return .skip(GM20BFacade.missing("switchMetalProbeComputeCta:", on: gpu.device))
        }
        defer {
            _ = Self.computeCta(gpu.device, false)
            DrawBandsTest.budget(gpu.device, production)
        }
        let kernel = try gpu.compute("pieces_kernel")
        let out = try gpu.buffer(Self.threads * 16)
        let encode = { (commands: MTLCommandBuffer) in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(kernel)
            encoder.setBuffer(out, offset: 0, index: 0)
            var steps = Self.steps
            encoder.setBytes(&steps, length: 4, index: 1)
            encoder.dispatchThreadgroups(
                MTLSize(width: Self.threads / Self.groupSize, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: Self.groupSize, height: 1, depth: 1)
            )
            encoder.endEncoding()
        }
        try await gpu.run(encode)
        let reference = Data(bytes: out.contents(), count: out.length)

        let layer = await context.onStage { stage -> CALayer in
            let spinner = CALayer()
            spinner.frame = CGRect(x: 20, y: 20, width: 80, height: 80)
            spinner.backgroundColor = UIColor.systemOrange.cgColor
            let spin = CABasicAnimation(keyPath: "transform.rotation.z")
            spin.fromValue = 0
            spin.toValue = 2 * Double.pi
            spin.duration = 1
            spin.repeatCount = .infinity
            spinner.add(spin, forKey: "spin")
            stage.layer.addSublayer(spinner)
            return spinner
        }

        var metrics = Metrics()
        var failures: [String] = []
        let connection = GPUConnection()
        for (arm, cta) in [("_wfi", false), ("_cta", true)] {
            let reply = Self.computeCta(gpu.device, cta) ?? "missing"
            let parsed = SpanReading.parse(reply)
            let want = cta ? 1 : 0
            if let old = parsed.counts["old"] { metrics.set("cta_old" + arm, old) }
            if let readback = parsed.counts["readback"] {
                metrics.set("cta_readback" + arm, readback)
                if readback != want {
                    failures.append("cta\(arm): option 0x60 read back \(readback), expected \(want)")
                }
            } else {
                failures.append("cta\(arm): switchMetalProbeComputeCta failed (\(reply))")
            }
            memset(out.contents(), 0, out.length)
            _ = GPUPaths.read(connection)
            let syncBefore = connection.flatMap { LaunchHitchTest.spans($0) }
            var dispatches = 0
            var totalMs = 0.0
            let until = CFAbsoluteTimeGetCurrent() + Self.kickSeconds
            while CFAbsoluteTimeGetCurrent() < until {
                let timing = try await gpu.run(encode)
                dispatches += 1
                totalMs += timing.gpuMilliseconds
            }
            metrics.set("dispatches" + arm, dispatches)
            if dispatches > 0 {
                metrics.set("us_per_dispatch" + arm, totalMs * 1000 / Double(dispatches), digits: 0)
            }
            let identical = Data(bytes: out.contents(), count: out.length) == reference
            metrics.set("identical" + arm, identical)
            if !identical { failures.append("cta\(arm): compute output differed from reference") }
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
                failures.append("cta\(arm): no compositor kick met the compute dispatch in \(Self.kickSeconds) s")
            }
        }
        await MainActor.run { layer.removeFromSuperlayer() }
        await context.clearStage()
        metrics.fitLog(within: 720, keeping: Self.judged)
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }

    static let judged: Set<String> = [
        "cta_readback_wfi", "cta_readback_cta", "identical_wfi", "identical_cta", "preempts_wfi", "preempts_cta",
        "preempt_max_us_wfi", "preempt_max_us_cta", "sync_max_us_wfi", "sync_max_us_cta", "us_per_dispatch_wfi",
        "us_per_dispatch_cta",
    ]

    static func supportsComputeCta(_ device: MTLDevice) -> Bool {
        let selector = NSSelectorFromString("switchMetalProbeComputeCta:")
        return (device as? NSObject)?.responds(to: selector) == true
    }

    @discardableResult
    static func computeCta(_ device: MTLDevice, _ on: Bool) -> String? {
        let selector = NSSelectorFromString("switchMetalProbeComputeCta:")
        guard let object = device as? NSObject, object.responds(to: selector) else { return nil }
        return object.perform(selector, with: NSNumber(value: on))?.takeUnretainedValue() as? String
    }
}
