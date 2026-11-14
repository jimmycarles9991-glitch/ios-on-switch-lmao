import Foundation
import HWTestCore
import HWTestGPU
import Metal
import QuartzCore

/// What a commit waits for (RawMetalPipeline.mm, debug builds): Core Animation shaped command buffers recorded at the
/// render thread's pace (a pass every 400 us, its draws binding vertices, transform and uniforms by setBytes under a
/// scissor of their own), each committed, waited for scheduled as the render server does before its swap, then for its
/// completion on an otherwise idle GPU. Per shape (one pass of 6 draws; 15 passes of 90 draws over 4 targets; the same
/// with a shared event signalled between passes 8 and 9; the same with its last pass into a drawable it presents): the
/// commit call's time, waitUntilScheduled's, and the time from the commit to the GPU starting its submission (its start
/// stamp on the CPU's clock), and how many of its encoders the submit worker encoded before the commit. A shape without
/// an event must have all but the encoders recorded since the worker was last handed the buffer (24 tape commands, 3
/// of these passes) and the two held for the look-ahead encoded ahead; its median from commit to GPU start must stay
/// under what the remainder, the kernel's SubmitJob and a thread hop take (the encode was 2.2-4.6 ms of a scale frame
/// before, boot 21); the event's shape is encoded at its commit, as before. Each pass draws into BGRA8 beside a
/// memoryless colour 1 in the format the state's line gives it (RGBA16Float on iOS 27). A pack without the state sends
/// its first command buffers to m2v while the runtime makes its record: those warm it before any shape runs.
struct CommitLatencyPathTest: HWTest {
    let id = "gpu.path.commit_latency"
    let name = "A Core Animation shaped command buffer's commit returns at once and its GPU work starts soon after"
    var subsystem: Subsystem { .metal }
    static let width = 256, height = 256, targets = 4, warmups = 6, measured = 16, stateWarmups = 20
    static let paceMicroseconds: UInt32 = 400
    /// The median from commit to the GPU's start a shape may take: the last encoders' encode (about a fifth of a
    /// 90-draw buffer's 0.9-1.4 ms loop), the job's SubmitJob (0.45-0.83 ms on boot 21's scale frames, being cut by
    /// x1-scale) and the worker's wake
    static let gpuStartMostUs: [String: Double] = ["one_pass": 1200, "scale": 1600, "scale_present": 1600]
    static let shapes: [(name: String, passes: Int, draws: Int, event: Bool, present: Bool)] = [
        ("one_pass", 1, 6, false, false), ("scale", 15, 90, false, false), ("scale_event", 15, 90, true, false),
        ("scale_present", 15, 90, false, true),
    ]

    func run(_: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        let library: MTLLibrary
        do {
            library = try gpu.device.makeLibrary(URL: CAState.library)
        } catch {
            return .skip("no QuartzCore library here (\(GPU.describe(error)))")
        }
        guard let line = CACorpus.state("PBGRAXm_A2Xhf") else { return .fail("CACorpus has no PBGRAXm_A2Xhf") }
        let pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
        let targets = try (0..<Self.targets).map { _ in try gpu.texture(.bgra8Unorm, Self.width, Self.height) }
        let scratch = try gpu.texture(
            CACorpus.colorFormat(line, 1), Self.width, Self.height, usage: [.renderTarget], storage: .memoryless)
        guard let event = gpu.device.makeSharedEvent() else { return .fail("makeSharedEvent returned nil") }
        let layer = CAMetalLayer()
        layer.device = gpu.device
        layer.pixelFormat = .bgra8Unorm
        layer.drawableSize = CGSize(width: Self.width, height: Self.height)
        var metrics = Metrics()
        var warm = 0
        while warm < Self.stateWarmups {
            guard let commands = gpu.queue.makeCommandBuffer() else { return .fail("makeCommandBuffer nil") }
            Self.encodePass(
                commands, pipeline: pipeline, target: targets[0], scratch: scratch, pass: 0, round: 0, clear: true,
                draws: 6)
            commands.commit()
            commands.waitUntilCompleted()
            if RawFacade.path(of: commands) == "raw" { break }
            warm += 1
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        metrics.set("warm", warm)
        var failures: [String] = []
        var signalled: UInt64 = 0
        for shape in Self.shapes {
            var commitUs: [Double] = [], scheduledUs: [Double] = [], gpuStartUs: [Double] = []
            var pipelined = 0.0, encoders = 0.0, abandons = 0.0, vetoes: UInt64 = 0, measuredCount = 0.0
            for round in 0..<(Self.warmups + Self.measured) {
                guard let commands = gpu.queue.makeCommandBuffer() else { return .fail("makeCommandBuffer nil") }
                let drawable = shape.present ? layer.nextDrawable() : nil
                if shape.present && drawable == nil { return .fail("nextDrawable returned nil") }
                for pass in 0..<shape.passes {
                    let last = pass == shape.passes - 1
                    let target = last && drawable != nil ? drawable!.texture : targets[pass % Self.targets]
                    Self.encodePass(
                        commands, pipeline: pipeline, target: target, scratch: scratch, pass: pass, round: round,
                        clear: pass < Self.targets || target !== targets[pass % Self.targets],
                        draws: shape.draws / shape.passes + (pass < shape.draws % shape.passes ? 1 : 0))
                    if shape.event && pass == 7 {
                        signalled += 1
                        commands.encodeSignalEvent(event, value: signalled)
                    }
                    usleep(Self.paceMicroseconds)
                }
                if let drawable { commands.present(drawable) }
                let committing = CACurrentMediaTime()
                commands.commit()
                let returned = CACurrentMediaTime()
                commands.waitUntilScheduled()
                let scheduled = CACurrentMediaTime()
                commands.waitUntilCompleted()
                if commands.status == .error {
                    failures.append("\(shape.name): \(commands.error.map(GPU.describe) ?? "error")")
                    break
                }
                if RawFacade.path(of: commands) != "raw" {
                    failures.append("\(shape.name) went \(RawFacade.path(of: commands))")
                    break
                }
                guard round >= Self.warmups else { continue }
                commitUs.append((returned - committing) * 1e6)
                scheduledUs.append((scheduled - returned) * 1e6)
                let ahead = RawFacade.pipelined(commands)
                if let start = ahead["commit_to_gpu_start_us"] { gpuStartUs.append(start.doubleValue) }
                pipelined += ahead["pipelined"]?.doubleValue ?? 0
                encoders += ahead["encoders"]?.doubleValue ?? 0
                abandons += ahead["abandons"]?.doubleValue ?? 0
                vetoes |= ahead["veto"]?.uint64Value ?? 0
                measuredCount += 1
            }
            let n = max(measuredCount, 1)
            metrics.set(shape.name + "_commit_us", Self.median(commitUs), digits: 0)
            metrics.set(shape.name + "_scheduled_wait_us", Self.median(scheduledUs), digits: 0)
            metrics.set(shape.name + "_scheduled_wait_us_p90", Self.percentile(scheduledUs, 0.9), digits: 0)
            metrics.set(shape.name + "_commit_to_gpu_start_us", Self.median(gpuStartUs), digits: 0)
            metrics.set(shape.name + "_commit_to_gpu_start_us_p90", Self.percentile(gpuStartUs, 0.9), digits: 0)
            metrics.set(shape.name + "_gpu_starts", gpuStartUs.count)
            metrics.set(shape.name + "_pipelined", pipelined / n, digits: 1)
            metrics.set(shape.name + "_encoders", encoders / n, digits: 1)
            metrics.set(shape.name + "_abandons", abandons / n, digits: 2)
            metrics.set(shape.name + "_veto", String(vetoes, radix: 16))
            if measuredCount == 0 { continue }
            if !shape.event && shape.passes > 3 && pipelined / n < encoders / n - 5 {
                failures.append(
                    "\(shape.name): \(pipelined / n) of \(encoders / n) encoders encoded ahead (veto \(vetoes))")
            }
            if shape.event && pipelined > 0 {
                failures.append("\(shape.name): encoders encoded ahead of a command buffer with an event")
            }
            if let most = Self.gpuStartMostUs[shape.name] {
                metrics.set(shape.name + "_commit_to_gpu_start_most_us", most, digits: 0)
                if gpuStartUs.count < Self.measured / 2 {
                    failures.append("\(shape.name): \(gpuStartUs.count) GPU start stamps read")
                } else if Self.median(gpuStartUs) > most {
                    failures.append(
                        "\(shape.name): commit to GPU start \(Int(Self.median(gpuStartUs))) us, over \(Int(most))")
                }
            }
        }
        let counters = RawFacade.counters(of: gpu.device)
        metrics.set("pipelined_encoders", counters["pipelined_encoders"] ?? 0)
        metrics.set("sealed_encoders", counters["sealed_encoders"] ?? 0)
        metrics.set(
            "pipeline_abandons",
            counters.filter { $0.key.hasPrefix("pipeline_abandons.") }.map { "\($0.key.dropFirst(18))=\($0.value)" }
                .sorted().joined(separator: ","))
        if counters["sealed_encoders"] == nil {
            return counters["raw"] == nil
                ? .skip("no pipelining counters: a release runtime")
                : .fail("a debug runtime without pipelining counters", metrics, proof: gpu.proof)
        }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }

    static func encodePass(
        _ commands: MTLCommandBuffer, pipeline: MTLRenderPipelineState, target: MTLTexture, scratch: MTLTexture,
        pass: Int, round: Int, clear: Bool, draws: Int
    ) {
        let descriptor = MTLRenderPassDescriptor()
        descriptor.colorAttachments[0].texture = target
        descriptor.colorAttachments[0].loadAction = clear ? .clear : .load
        descriptor.colorAttachments[0].storeAction = .store
        descriptor.colorAttachments[1].texture = scratch
        descriptor.colorAttachments[1].loadAction = .clear
        descriptor.colorAttachments[1].storeAction = .dontCare
        let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)!
        encoder.setRenderPipelineState(pipeline)
        for draw in 0..<draws {
            let x = Float((draw * 37 + round) % 97) / 97 - 0.5, y = Float(pass % 8) / 4 - 1
            let colour = SIMD4<Float16>(Float16(draw % 4) / 4, Float16(pass % 8) / 8, 0.5, 1)
            CAState.bind(
                encoder, vertices: CAState.rectangle(x0: x, y0: y, x1: x + 0.25, y1: y + 0.3, color: colour),
                source: nil)
            encoder.setScissorRect(MTLScissorRect(x: (draw % 8) * 32, y: (pass % 8) * 32, width: 32, height: 64))
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        }
        encoder.endEncoding()
    }

    static func percentile(_ values: [Double], _ p: Double) -> Double {
        guard !values.isEmpty else { return 0 }
        let sorted = values.sorted()
        return sorted[min(sorted.count - 1, Int(p * Double(sorted.count)))]
    }

    static func median(_ values: [Double]) -> Double { percentile(values, 0.5) }
}
