import Foundation
import HWTestCore
import HWTestGPU
import Metal
import QuartzCore
import UIKit

/// An app draw longer than a band (TegraGM20BSchedPolicy.h TGM_APP_BAND_NS) drawn in scissor bands
/// (raw/runtime/raw_draw_bands.h). A layer animates on screen throughout, so the compositor kicks every frame and
/// preempts these draws. Per case (blended, framebuffer fetch inside the pixel interlock, 4x MSAA resolved) the same two
/// full-screen draws of a ~15 ms fragment program are made banded and with the facade's band budget at 0: the bytes
/// must be identical and only the first arm banded (banded_draws, bands); the fetch case must have drawn interlocked
/// (interlock_draws), so its identity covers interlock tickets across bands and preempts. Then banded draws run back to
/// back: the kernel's preemption latencies sampled for this process (debug kernels, TegraGM20BPathsABI.h `preempts`,
/// `preempt_max_us`, buckets `preempt_lt250_us`..`preempt_ge4000_us`) must stay within the budget plus a context
/// switch, and where each preempt landed (SwitchGpuPreemptLanding.h): `preempt_mid_job` at a band's release inside a
/// job, `preempt_job_end` at a job's end. One mid-job landing proves a preempt lands while the PBDMA waits at a band's
/// release. The compositor's profiled submissions kicked behind these draws (spans op: `behind`, `sync_max_us`) must find GR
/// within the same bound: their first push's wait for GR is the app's preemption latency. The unbanded arm shows what a whole
/// draw costs the compositor. us_per_band is the GPU time a band took.
/// The learned arm draws a loop of scattered texture reads, a few dozen instructions priced far under a band: its first
/// draw is whole and measured (raw_band_rates.h), and once its state's rate is learned the same draw must be banded by
/// the time it took, with the whole draw's pixels. Its draw is alone in its pass, so the state's first three are measured and
/// the rest not (raw_band_rates.h): us_measured (the measured banded draws) against us_unmeasured (the later ones) is what a
/// measurement costs the GPU, which must be nothing beyond noise.
struct DrawBandsTest: HWTest {
    let id = "gpu.sched.long_draw_bands"
    let name = "Long app draws in scissor bands: compositor wait and pixel identity"
    let subsystem = Subsystem.metal

    static let width = 1280
    static let height = 720
    static let draws = 2
    static let budgetUs = 1000
    static let waitBoundUs = 2000
    static let kickSeconds = 1.5
    static let memorySide = 2048
    static let memoryReads: UInt32 = 8
    static let learnRounds = 7
    static let measuredSlackUs = 50.0

    struct Case {
        let name: String
        let fragment: String
        let samples: Int
        let blend: Bool
    }

    static let cases = [
        Case(name: "blend", fragment: "bands_blend_fragment", samples: 1, blend: true),
        Case(name: "fetch", fragment: "bands_fetch_fragment", samples: 1, blend: false),
        Case(name: "msaa", fragment: "bands_blend_fragment", samples: 4, blend: true),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        guard let production = Self.budget(gpu.device, Self.budgetUs * 1000) else {
            return .skip("the facade has no band budget (a release build)")
        }
        defer { Self.budget(gpu.device, production) }
        var metrics = Metrics()
        var failures: [String] = []
        let layer = await context.onStage { stage -> CALayer in
            let spinner = CALayer()
            spinner.frame = CGRect(x: 20, y: 20, width: 80, height: 80)
            spinner.backgroundColor = UIColor.systemTeal.cgColor
            let spin = CABasicAnimation(keyPath: "transform.rotation.z")
            spin.fromValue = 0
            spin.toValue = 2 * Double.pi
            spin.duration = 1
            spin.repeatCount = .infinity
            spinner.add(spin, forKey: "spin")
            stage.layer.addSublayer(spinner)
            return spinner
        }
        for test in Self.cases {
            let pipeline: MTLRenderPipelineState
            do {
                pipeline = try gpu.render(
                    vertex: "bands_vertex", fragment: test.fragment, format: .bgra8Unorm, samples: test.samples
                ) { descriptor in
                    let target = descriptor.colorAttachments[0]!
                    target.isBlendingEnabled = test.blend
                    target.sourceRGBBlendFactor = .sourceAlpha
                    target.destinationRGBBlendFactor = .oneMinusSourceAlpha
                }
            } catch {
                await MainActor.run { layer.removeFromSuperlayer() }
                await context.clearStage()
                return .skip("no \(test.name) pipeline: \(GPU.describe(error))", metrics)
            }
            let banded = try await draw(gpu, pipeline, test, metrics: &metrics, arm: "")
            _ = Self.budget(gpu.device, 0)
            let whole = try await draw(gpu, pipeline, test, metrics: &metrics, arm: "_whole")
            _ = Self.budget(gpu.device, Self.budgetUs * 1000)
            let identical = banded.bytes == whole.bytes
            metrics.set("identical_" + test.name, identical)
            if !identical { failures.append("\(test.name): banded pixels differ from the whole draw's") }
            if banded.bands < 2 * Self.draws {
                failures.append("\(test.name): \(banded.bands) bands for \(Self.draws) draws")
            }
            if whole.bands != 0 { failures.append("\(test.name): \(whole.bands) bands with the budget at 0") }
            if test.name == "fetch" && banded.interlocked == 0 {
                failures.append("fetch: no draw ran interlocked, so its identity says nothing of the interlock")
            }
        }
        failures += try await learned(gpu, metrics: &metrics)
        let blend = Self.cases[0]
        let pipeline = try gpu.render(vertex: "bands_vertex", fragment: blend.fragment, format: .bgra8Unorm) { _ in }
        let target = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        let connection = GPUConnection()
        for (arm, budgetNs) in [("", Self.budgetUs * 1000), ("_whole", 0)] {
            _ = Self.budget(gpu.device, budgetNs)
            _ = GPUPaths.read(connection)
            let syncBefore = connection.flatMap { LaunchHitchTest.spans($0) }
            let until = CFAbsoluteTimeGetCurrent() + Self.kickSeconds
            while CFAbsoluteTimeGetCurrent() < until {
                try await gpu.run { commands in Self.encode(commands, pipeline, target, samples: 1, resolve: nil) }
            }
            guard case .counts(let after) = GPUPaths.read(connection) else {
                failures.append("the kernel's paths line is unreadable: no preemption latencies")
                break
            }
            let samples = after.counts["preempts"] ?? 0
            let maxUs = after.counts["preempt_max_us"] ?? 0
            if syncBefore != nil, let line = connection.flatMap({ LaunchHitchTest.spans($0) }) {
                let sync = SpanReading.parse(line)
                for key in ["sync_subs", "behind", "behind_sync_us", "sync_max_us"] {
                    metrics.set(key + arm, sync.counts[key] ?? 0)
                }
                if arm.isEmpty && (sync.counts["behind"] ?? 0) > 0
                    && (sync.counts["sync_max_us"] ?? 0) > Self.waitBoundUs
                {
                    failures.append(
                        "a compositor submission's sync push waited \(sync.counts["sync_max_us"] ?? 0) us for GR")
                }
            }
            for key in Self.preemptKeys { metrics.set(key + arm, after.counts[key] ?? 0) }
            if arm.isEmpty {
                if samples == 0 { failures.append("no compositor kick met a banded draw in \(Self.kickSeconds) s") }
                if maxUs > Self.waitBoundUs {
                    failures.append("the compositor waited \(maxUs) us behind a banded draw")
                }
                if samples > 0 && (after.counts["preempt_mid_job"] ?? 0) == 0 {
                    failures.append("no preempt landed at a band's release inside a job")
                }
            }
        }
        await MainActor.run { layer.removeFromSuperlayer() }
        await context.clearStage()
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }

    func learned(_ gpu: GPU, metrics: inout Metrics) async throws -> [String] {
        let pipeline = try gpu.render(vertex: "bands_vertex", fragment: "bands_memory_fragment", format: .bgra8Unorm)
        let side = Self.memorySide
        let big = try gpu.texture(.rgba16Float, side, side, usage: [.shaderRead], storage: .shared)
        var halves = [UInt16](repeating: 0, count: side * side * 4)
        var seed: UInt32 = 0x2545_F491
        for i in halves.indices {
            seed = seed &* 1_664_525 &+ 1_013_904_223
            halves[i] = 0x3000 | UInt16(seed >> 22)
        }
        halves.withUnsafeBytes {
            big.replace(
                region: MTLRegionMake2D(0, 0, side, side), mipmapLevel: 0, withBytes: $0.baseAddress!,
                bytesPerRow: side * 8)
        }
        let target = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        let encode = { (commands: MTLCommandBuffer) in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setFragmentTexture(big, index: 0)
            var reads = Self.memoryReads
            encoder.setFragmentBytes(&reads, length: 4, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            encoder.endEncoding()
        }
        var failures: [String] = []
        var wholeMs = 0.0
        var learnedBands = 0
        var measuredUs: [Double] = [], unmeasuredUs: [Double] = []
        for round in 0..<Self.learnRounds {
            let before = RawFacade.counters(of: gpu.device)
            let timing = try await gpu.run(encode)
            let after = RawFacade.counters(of: gpu.device)
            let bands = (after["bands"] ?? 0) - (before["bands"] ?? 0)
            let learned = (after["learned_draws"] ?? 0) - (before["learned_draws"] ?? 0)
            if round == 0 {
                wholeMs = timing.gpuMilliseconds
                metrics.set("path_memory", timing.path)
                metrics.set("us_memory_first", timing.gpuMilliseconds * 1000, digits: 0)
                metrics.set("bands_memory_first", bands)
                metrics.set("learned_memory_first", learned)
                if learned < 1 { failures.append("memory: the first draw's GPU time taught its state nothing") }
            } else {
                learnedBands = bands
                metrics.set("us_memory_learned", timing.gpuMilliseconds * 1000, digits: 0)
                if timing.gpuTimed {
                    if learned > 0 {
                        measuredUs.append(timing.gpuMilliseconds * 1000)
                    } else {
                        unmeasuredUs.append(timing.gpuMilliseconds * 1000)
                    }
                }
            }
        }
        metrics.set("bands_memory_learned", learnedBands)
        metrics.set("measured_draws", measuredUs.count)
        metrics.set("unmeasured_draws", unmeasuredUs.count)
        if let measured = Self.mean(measuredUs), let unmeasured = Self.mean(unmeasuredUs) {
            metrics.set("us_measured", measured, digits: 0)
            metrics.set("us_unmeasured", unmeasured, digits: 0)
            if measured > unmeasured * 1.02 + Self.measuredSlackUs {
                failures.append(
                    "memory: a measured draw took \(Int(measured)) us, an unmeasured one \(Int(unmeasured)) us")
            }
        } else {
            failures.append(
                "memory: \(measuredUs.count) measured and \(unmeasuredUs.count) unmeasured draws to compare")
        }
        let banded = try await gpu.read(target, bytesPerPixel: 4)
        _ = Self.budget(gpu.device, 0)
        try await gpu.run(encode)
        _ = Self.budget(gpu.device, Self.budgetUs * 1000)
        let identical = banded == (try await gpu.read(target, bytesPerPixel: 4))
        metrics.set("identical_memory", identical)
        if !identical { failures.append("memory: banded pixels differ from the whole draw's") }
        if wholeMs * 1000 > Double(2 * Self.budgetUs) && learnedBands < 2 {
            failures.append("memory: a \(Int(wholeMs * 1000)) us draw went in \(learnedBands) bands once learned")
        }
        return failures
    }
    static func mean(_ values: [Double]) -> Double? {
        values.isEmpty ? nil : values.reduce(0, +) / Double(values.count)
    }

    static let preemptKeys = [
        "preempts", "preempt_max_us", "preempt_mid_job", "preempt_job_end", "preempt_unknown", "preempt_lt250_us",
        "preempt_lt500_us", "preempt_lt1000_us", "preempt_lt2000_us", "preempt_lt4000_us", "preempt_ge4000_us",
    ]

    struct Drawn {
        let bytes: [UInt8]
        let bands: Int
        let interlocked: Int
    }

    func draw(
        _ gpu: GPU, _ pipeline: MTLRenderPipelineState, _ test: Case, metrics: inout Metrics, arm: String
    ) async throws -> Drawn {
        let target = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        let msaa = test.samples > 1 ? try gpu.texture(.bgra8Unorm, Self.width, Self.height, samples: test.samples) : nil
        let before = RawFacade.counters(of: gpu.device)
        let timing = try await gpu.run { commands in
            Self.encode(commands, pipeline, msaa ?? target, samples: test.samples, resolve: msaa == nil ? nil : target)
        }
        let after = RawFacade.counters(of: gpu.device)
        let drawsBanded = (after["banded_draws"] ?? 0) - (before["banded_draws"] ?? 0)
        let bands = (after["bands"] ?? 0) - (before["bands"] ?? 0)
        let interlocked = (after["interlock_draws"] ?? 0) - (before["interlock_draws"] ?? 0)
        let key = test.name + arm
        metrics.set("interlock_draws_" + key, interlocked)
        metrics.set("path_" + key, timing.path)
        metrics.set("banded_draws_" + key, drawsBanded)
        metrics.set("bands_" + key, bands)
        if timing.gpuTimed {
            metrics.set("us_per_draw_" + key, timing.gpuMilliseconds * 1000 / Double(Self.draws), digits: 0)
            if bands > 0 { metrics.set("us_per_band_" + key, timing.gpuMilliseconds * 1000 / Double(bands), digits: 0) }
        }
        return Drawn(bytes: try await gpu.read(target, bytesPerPixel: 4), bands: bands, interlocked: interlocked)
    }

    static func encode(
        _ commands: MTLCommandBuffer, _ pipeline: MTLRenderPipelineState, _ target: MTLTexture, samples: Int,
        resolve: MTLTexture?
    ) {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1)
        pass.colorAttachments[0].resolveTexture = resolve
        pass.colorAttachments[0].storeAction = resolve == nil ? .store : .multisampleResolve
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(pipeline)
        for _ in 0..<draws { encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3) }
        encoder.endEncoding()
    }

    /// Sets the facade's band budget (debug builds) and answers the one it replaced; nil off a debug facade.
    @discardableResult
    static func budget(_ device: MTLDevice, _ ns: Int) -> Int? {
        let selector = NSSelectorFromString("switchRawBandBudget:")
        guard let object = device as? NSObject, object.responds(to: selector) else { return nil }
        let old = object.perform(selector, with: NSNumber(value: ns))?.takeUnretainedValue() as? String
        return old.flatMap { Int($0) }
    }
}
