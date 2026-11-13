import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// State segments (drivers/TegraGM20BMetal/raw/runtime/raw_state_segment.h): a pipeline's registers are written once
/// into GPU memory at its creation and a bind runs them from there, and a pass that starts with the pipeline the last
/// pass ended with writes nothing for it. Frames of two passes, each pass an opaque quad, an additive quad over it and
/// the opaque quad again, so every pass binds A, B, A and starts with the A the pass or frame before ended with.
/// Additive blending of 8-bit values is exact, so every pixel is checked against its sum. Then the same command buffers
/// of `switches` alternating binds, in batches bound inline (as before segments) and by segment in turn: the encode
/// time the facade counted, its pushes and kernel submit time per command buffer, and the GPU time per command buffer
/// of each, which decide whether a state as short as these is worth a segment (raw_state_segment.h kSegmentBindWords),
/// and both arms' pixels, the same bytes. A state the runtime made with registers outside the pack's (extras) cannot
/// keep its segment across a pass, which the facade counts apart. The counters are the facade's (debug builds); a
/// release runtime has none and the test skips.
struct StateSegmentsPathTest: HWTest {
    let id = "gpu.path.state_segments"
    let name = "Pipeline binds run GPU-resident state segments, and a held one is not bound again"
    var subsystem: Subsystem { .metal }
    static let frames = 8, width = 16, height = 16, switches = 128, batches = 4, perBatch = 6

    static func opaque(_ frame: Int) -> [UInt8] { [UInt8(10 + frame), 40, UInt8(100 - frame), 255] }
    static let added: [UInt8] = [20, 30, 40, 0]

    func run(_: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        let before = RawFacade.counters(of: gpu.device)
        guard before["state_segment_binds"] != nil else { return .skip("no state segment counters: a release runtime") }
        var metrics = Metrics()
        let a = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        let b = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm) { descriptor in
            let colour = descriptor.colorAttachments[0]!
            colour.isBlendingEnabled = true
            colour.sourceRGBBlendFactor = .one
            colour.destinationRGBBlendFactor = .one
            colour.sourceAlphaBlendFactor = .zero
            colour.destinationAlphaBlendFactor = .one
        }
        var targets: [MTLTexture] = []
        var committed: [MTLCommandBuffer] = []
        for frame in 0..<Self.frames {
            let target = try gpu.texture(.rgba8Unorm, Self.width, Self.height)
            targets.append(target)
            committed.append(
                try gpu.commit { commands in
                    for pass in 0..<2 {
                        let descriptor = MTLRenderPassDescriptor()
                        descriptor.colorAttachments[0].texture = target
                        descriptor.colorAttachments[0].loadAction = pass == 0 ? .clear : .load
                        descriptor.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                        descriptor.colorAttachments[0].storeAction = .store
                        let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)!
                        Self.quad(encoder, a, Self.opaque(frame), x: 0)
                        Self.quad(encoder, b, Self.added, x: 0)
                        Self.quad(encoder, a, Self.opaque(frame), x: Self.width / 2)
                        encoder.endEncoding()
                    }
                })
        }
        try gpu.finish(committed)
        var wrong = 0
        for (frame, target) in targets.enumerated() {
            let bytes = try await gpu.read(target, bytesPerPixel: 4)
            let left = zip(Self.opaque(frame), Self.added).map { UInt8(min(255, Int($0) + Int($1))) }
            for y in 0..<Self.height {
                for x in 0..<Self.width {
                    let at = (y * Self.width + x) * 4
                    wrong += Self.wrong(Array(bytes[at..<(at + 4)]), x < Self.width / 2 ? left : Self.opaque(frame))
                }
            }
        }
        let after = RawFacade.counters(of: gpu.device)
        func taken(_ key: String) -> Int { (after[key] ?? 0) - (before[key] ?? 0) }
        let binds = taken("state_segment_binds"), skips = taken("state_segment_skips")
        let words = taken("state_segment_words"), extras = taken("state_segment_extras_losses")
        metrics.set("frames", Self.frames)
        metrics.set("wrong_bytes", wrong)
        metrics.set("segment_binds", binds)
        metrics.set("inline_words_saved", words)
        metrics.set("redundant_binds_skipped", skips)
        metrics.set("held_lost_to_extras", extras)
        metrics.set("words_per_bind", binds > 0 ? String(format: "%.1f", Double(words) / Double(binds)) : "-")
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        var failures = try await compare(gpu, a, b, &metrics)
        if gpu.paths.paths.keys.contains(where: { $0 != "raw" }) {
            failures.append("command buffers went \(gpu.paths.text)")
        }
        if wrong != 0 { failures.append("\(wrong) bytes differ from the quads' colours and sums") }
        if binds < 2 * Self.frames { failures.append("\(binds) segment binds for \(Self.frames) frames of A, B, A") }
        if words <= binds { failures.append("the binds replaced \(words) inline words") }
        if skips < Self.frames && extras == 0 { failures.append("\(skips) binds of the held segment skipped") }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }

    func compare(
        _ gpu: GPU, _ a: MTLRenderPipelineState, _ b: MTLRenderPipelineState, _ metrics: inout Metrics
    )
        async throws -> [String]
    {
        var encodeUs = [0.0, 0.0], gpuUs = [0.0, 0.0], execUs = [0.0, 0.0], encodes = [0, 0], pushes = [0, 0]
        var images: [[UInt8]] = [[], []]
        for batch in 0..<Self.batches {
            let arm = batch % 2
            let before = RawFacade.counters(of: gpu.device)
            var buffers: [MTLCommandBuffer] = []
            var target: MTLTexture?
            for _ in 0..<Self.perBatch {
                let drawn = try gpu.texture(.rgba8Unorm, Self.width, Self.height)
                target = drawn
                buffers.append(
                    try gpu.commit { commands in
                        if arm == 0 { _ = RawFacade.inlineStates(in: commands) }
                        let descriptor = MTLRenderPassDescriptor()
                        descriptor.colorAttachments[0].texture = drawn
                        descriptor.colorAttachments[0].loadAction = .clear
                        descriptor.colorAttachments[0].storeAction = .store
                        let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)!
                        for i in 0..<Self.switches {
                            Self.quad(encoder, i % 2 == 0 ? a : b, i % 2 == 0 ? Self.opaque(i % 16) : Self.added, x: 0)
                        }
                        encoder.endEncoding()
                    })
            }
            try gpu.finish(buffers)
            let after = RawFacade.counters(of: gpu.device)
            encodeUs[arm] += Double((after["encode_us"] ?? 0) - (before["encode_us"] ?? 0))
            encodes[arm] += (after["encodes"] ?? 0) - (before["encodes"] ?? 0)
            execUs[arm] += Double((after["exec_us"] ?? 0) - (before["exec_us"] ?? 0))
            pushes[arm] += (after["exec_pushes"] ?? 0) - (before["exec_pushes"] ?? 0)
            gpuUs[arm] += buffers.map { max(0, $0.gpuEndTime - $0.gpuStartTime) * 1e6 }.reduce(0, +)
            if images[arm].isEmpty, let target { images[arm] = try await gpu.read(target, bytesPerPixel: 4) }
        }
        let buffers = Double(Self.batches / 2 * Self.perBatch)
        let perEncode = (0..<2).map { encodes[$0] > 0 ? encodeUs[$0] / Double(encodes[$0]) : 0 }
        metrics.set("inline_encode_us", String(format: "%.0f", perEncode[0]))
        metrics.set("segment_encode_us", String(format: "%.0f", perEncode[1]))
        let perBind = (perEncode[1] - perEncode[0]) * 1000 / Double(Self.switches)
        metrics.set("segment_cpu_ns_per_bind", String(format: "%.0f", perBind))
        let perCb = { (values: [Double], arm: Int) in encodes[arm] > 0 ? values[arm] / Double(encodes[arm]) : 0 }
        let pushesPerCb = (0..<2).map { perCb(pushes.map(Double.init), $0) }
        metrics.set("inline_pushes_per_cb", String(format: "%.0f", pushesPerCb[0]))
        metrics.set("segment_pushes_per_cb", String(format: "%.0f", pushesPerCb[1]))
        metrics.set("inline_submit_us", String(format: "%.0f", perCb(execUs, 0)))
        metrics.set("segment_submit_us", String(format: "%.0f", perCb(execUs, 1)))
        let extraPushes = pushesPerCb[1] - pushesPerCb[0]
        metrics.set(
            "submit_ns_per_extra_push",
            extraPushes > 0 ? String(format: "%.0f", (perCb(execUs, 1) - perCb(execUs, 0)) * 1000 / extraPushes) : "-")
        metrics.set("inline_gpu_us", String(format: "%.0f", gpuUs[0] / buffers))
        metrics.set("segment_gpu_us", String(format: "%.0f", gpuUs[1] / buffers))
        let differ = Self.wrong(images[1], images[0])
        metrics.set("arms_differ_bytes", differ)
        return differ == 0 ? [] : ["\(differ) bytes differ between the inline and the segment binds"]
    }

    static func wrong(_ got: [UInt8], _ want: [UInt8]) -> Int {
        zip(got, want).filter { $0 != $1 }.count + abs(got.count - want.count)
    }

    static func quad(
        _ encoder: MTLRenderCommandEncoder, _ pipeline: MTLRenderPipelineState, _ colour: [UInt8], x: Int
    ) {
        encoder.setRenderPipelineState(pipeline)
        var corners = GPU.pixelQuad(x, 0, x + width / 2, height, width: width, height: height)
        var value = colour.map { Float($0) / 255 }
        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        encoder.setFragmentBytes(&value, length: MemoryLayout<Float>.stride * 4, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    }
}
