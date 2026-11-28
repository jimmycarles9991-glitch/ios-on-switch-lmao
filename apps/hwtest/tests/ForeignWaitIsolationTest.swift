import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import Metal
import QuartzCore
import UIKit

/// Drives back-to-back >30 ms draws on a visible CAMetalLayer via `[cb presentDrawable:]` beside a spinning Core
/// Animation layer for 2 s and asserts the kernel records zero compositor foreign waits (`d_foreign_jobs == 0`) while
/// `backboardd` keeps compositing.
struct ForeignWaitIsolationTest: HWTest {
    let id = "gpu.sched.foreign_wait_isolation"
    let name = "Heavy CAMetalLayer draws never force compositor foreign waits"
    let subsystem = Subsystem.metal

    static let width = DrawBandsTest.width
    static let height = DrawBandsTest.height
    static let drawsPerFrame = 3
    static let runSeconds = 2.0
    static let minFrames = 5
    static let minGpuMaxMs = 20.0
    static let minCompJobs = 20

    final class Tally: @unchecked Sendable {
        private let lock = NSLock()
        private(set) var presented = 0
        private(set) var completed = 0
        private(set) var failed = 0
        private(set) var totalGpuMs = 0.0
        private(set) var maxGpuMs = 0.0

        func notePresented() {
            lock.lock()
            presented += 1
            lock.unlock()
        }

        func noteCompleted(gpuMs: Double, failed: Bool) {
            lock.lock()
            completed += 1
            if failed { self.failed += 1 }
            if gpuMs > 0 {
                totalGpuMs += gpuMs
                if gpuMs > maxGpuMs { maxGpuMs = gpuMs }
            }
            lock.unlock()
        }

        func snapshot() -> (presented: Int, completed: Int, failed: Int, totalGpuMs: Double, maxGpuMs: Double) {
            lock.lock()
            defer { lock.unlock() }
            return (presented, completed, failed, totalGpuMs, maxGpuMs)
        }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        let connection = GPUConnection()
        guard GPUPaths.foreignWaits(connection) != nil else {
            return .skip(
                KernelSysctl.isReleaseKernel()
                    ? "release kernel: no compositor foreign-wait counters"
                    : "no compositor foreign-wait counters (build with --//bazel:stats_gpu)"
            )
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

        let (metalLayer, spinner) = await context.onStage { stage -> (CAMetalLayer, CALayer) in
            let metal = CAMetalLayer()
            metal.device = gpu.device
            metal.pixelFormat = .bgra8Unorm
            metal.framebufferOnly = true
            metal.maximumDrawableCount = 3
            metal.drawableSize = CGSize(width: Self.width, height: Self.height)
            metal.frame = CGRect(x: 0, y: 0, width: 320, height: 180)
            stage.layer.addSublayer(metal)

            let spinLayer = CALayer()
            spinLayer.frame = CGRect(x: 20, y: 200, width: 80, height: 80)
            spinLayer.backgroundColor = UIColor.systemGreen.cgColor
            let spin = CABasicAnimation(keyPath: "transform.rotation.z")
            spin.fromValue = 0
            spin.toValue = 2 * Double.pi
            spin.duration = 1
            spin.repeatCount = .infinity
            spinLayer.add(spin, forKey: "spin")
            stage.layer.addSublayer(spinLayer)
            return (metal, spinLayer)
        }

        autoreleasepool {
            if let warmDrawable = metalLayer.nextDrawable(),
               let warmCb = gpu.queue.makeCommandBuffer()
            {
                Self.encodeHeavy(warmCb, pipeline: pipeline, target: warmDrawable.texture, frame: 0)
                warmCb.present(warmDrawable)
                warmCb.commit()
                warmCb.waitUntilCompleted()
            }
        }
        try await Task.sleep(nanoseconds: 100_000_000)

        guard let before = GPUPaths.foreignWaits(connection) else {
            await MainActor.run {
                metalLayer.removeFromSuperlayer()
                spinner.removeFromSuperlayer()
            }
            await context.clearStage()
            return .skip("foreign-wait counters became unreadable")
        }
        let spansBefore = connection.flatMap { LaunchHitchTest.spans($0) }

        let tally = Tally()
        var frames = 0
        var nilDrawables = 0
        do {
            let inFlight = DispatchSemaphore(value: 2)
            var lastCb: MTLCommandBuffer?
            defer {
                lastCb?.waitUntilCompleted()
                for _ in 0 ..< 2 { inFlight.wait() }
                for _ in 0 ..< 2 { inFlight.signal() }
            }
            let until = CFAbsoluteTimeGetCurrent() + Self.runSeconds
            while CFAbsoluteTimeGetCurrent() < until {
                var submitted = false
                autoreleasepool {
                    inFlight.wait()
                    guard let drawable = metalLayer.nextDrawable(),
                          let cb = gpu.queue.makeCommandBuffer()
                    else {
                        inFlight.signal()
                        nilDrawables += 1
                        return
                    }
                    frames += 1
                    Self.encodeHeavy(cb, pipeline: pipeline, target: drawable.texture, frame: frames)
                    drawable.addPresentedHandler { _ in tally.notePresented() }
                    cb.addCompletedHandler { done in
                        let gpuMs = (done.gpuEndTime - done.gpuStartTime) * 1000.0
                        tally.noteCompleted(gpuMs: gpuMs, failed: done.status != .completed)
                        inFlight.signal()
                    }
                    cb.present(drawable)
                    cb.commit()
                    lastCb = cb
                    submitted = true
                }
                if !submitted {
                    try await Task.sleep(nanoseconds: 2_000_000)
                }
            }
        }
        try await Task.sleep(nanoseconds: 100_000_000)

        let after = GPUPaths.foreignWaits(connection)
        let spansAfter = spansBefore != nil ? connection.flatMap { LaunchHitchTest.spans($0) } : nil

        await MainActor.run {
            metalLayer.removeFromSuperlayer()
            spinner.removeFromSuperlayer()
        }
        await context.clearStage()

        let snap = tally.snapshot()
        var metrics = Metrics()
        var failures: [String] = []
        metrics.set("frames", frames)
        metrics.set("presented", snap.presented)
        metrics.set("nil_drawables", nilDrawables)
        metrics.set("failed_cbs", snap.failed)
        if snap.completed > 0 {
            metrics.set("gpu_mean_ms", snap.totalGpuMs / Double(snap.completed), digits: 1)
        }
        metrics.set("gpu_max_ms", snap.maxGpuMs, digits: 1)

        guard let after else {
            return .fail("foreign-wait counters unreadable after run", metrics)
        }
        let dJobs = (after["foreign_jobs"] ?? 0) - (before["foreign_jobs"] ?? 0)
        let dFences = (after["foreign_fences"] ?? 0) - (before["foreign_fences"] ?? 0)
        let dPromises = (after["foreign_promises"] ?? 0) - (before["foreign_promises"] ?? 0)
        let dEvents = (after["foreign_events"] ?? 0) - (before["foreign_events"] ?? 0)
        let dHost1x = (after["foreign_host1x"] ?? 0) - (before["foreign_host1x"] ?? 0)
        metrics.set("d_foreign_jobs", dJobs)
        metrics.set("d_foreign_fences", dFences)
        metrics.set("d_foreign_promises", dPromises)
        metrics.set("d_foreign_events", dEvents)
        metrics.set("d_foreign_host1x", dHost1x)

        var compJobs = 0
        if let line = spansAfter {
            let sync = SpanReading.parse(line)
            compJobs = sync.counts["jobs"] ?? 0
            metrics.set("comp_jobs", compJobs)
            for key in ["sync_subs", "behind", "sync_max_us", "windows", "holds"] {
                metrics.set(key, sync.counts[key] ?? 0)
            }
        }

        if frames < Self.minFrames {
            failures.append("only \(frames) heavy frames submitted in \(Self.runSeconds) s")
        }
        if snap.failed != 0 {
            failures.append("\(snap.failed) command buffers failed")
        }
        if snap.presented < 1 {
            failures.append("no CAMetalLayer drawable was presented")
        }
        if snap.maxGpuMs < Self.minGpuMaxMs {
            let msg = String(
                format: "max GPU draw %.1f ms, below %.0f ms floor", snap.maxGpuMs, Self.minGpuMaxMs
            )
            failures.append(msg)
        }
        if spansAfter != nil && compJobs < Self.minCompJobs {
            failures.append("compositor ran only \(compJobs) jobs during \(Self.runSeconds) s spin")
        }
        if dJobs != 0 {
            let counts = "\(dFences) fences, \(dPromises) promises, \(dEvents) events"
            failures.append("compositor waited on \(dJobs) foreign jobs (\(counts))")
        }

        metrics.fitLog(within: 720, keeping: Self.judged)
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }

    static let judged: Set<String> = [
        "frames", "presented", "gpu_mean_ms", "gpu_max_ms", "d_foreign_jobs", "d_foreign_fences",
        "d_foreign_promises", "d_foreign_events", "comp_jobs", "sync_max_us", "behind",
    ]

    static func encodeHeavy(
        _ commands: MTLCommandBuffer, pipeline: MTLRenderPipelineState, target: MTLTexture, frame: Int
    ) {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        let level = Double(frame % 60) / 60.0
        pass.colorAttachments[0].clearColor = MTLClearColor(red: level, green: 0.35, blue: 1.0 - level, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(pipeline)
        for _ in 0 ..< Self.drawsPerFrame {
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        }
        encoder.endEncoding()
    }
}
