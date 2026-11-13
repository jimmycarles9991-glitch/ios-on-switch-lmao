import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The GPU's time per full-screen draw that fetches the colour it blends into (fetch_blend_fragment under One/One
/// blending, a 1280x720 BGRA8 quad), `draws` of them in one command buffer, the median of `runs` once the state is
/// made: us_per_draw. The same draws with the same shader and blending off (us_per_draw_noblend), and with a shader that
/// fetches nothing under the same blend (us_per_draw_nofetch). path<variant> is where each was drawn, fetch= how the
/// raw facade's fetch counters say it kept order, gpu_clock as gpu.ca.hot_states (m2v's timestamps are PTIMER units).
/// A measurement: it passes once the draws ran, and skips where the device makes no such pipeline or keeps no GPU
/// timestamps.
struct FetchBlendCostTest: HWTest {
    let id = "gpu.raw.fetch_blend_cost"
    let name = "Self-blending framebuffer fetch: GPU time per full-screen draw"
    let subsystem = Subsystem.metal

    static let width = 1280
    static let height = 720
    static let draws = 20
    static let runs = 5
    static let warmups = 20

    struct Variant {
        let suffix: String
        let pipeline: MTLRenderPipelineState
        /// fetch_blend_fragment's scale, or solid_fragment's colour
        let constant: SIMD4<Float>
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        var metrics = Metrics()
        metrics.set("dev", kind)
        metrics.set("gpu_clock", kind == "m2v" ? "ptimer" : "ns")
        metrics.set("draws", Self.draws)
        let variants: [Variant]
        do {
            let scale = SIMD4<Float>(255, 0, 0, 0)
            let make = { (fragment: String, blend: Bool) in
                try FetchBlendPipeline.make(gpu, fragment: fragment, format: .bgra8Unorm, blend: blend)
            }
            variants = [
                Variant(suffix: "", pipeline: try make("fetch_blend_fragment", true), constant: scale),
                Variant(suffix: "_noblend", pipeline: try make("fetch_blend_fragment", false), constant: scale),
                Variant(
                    suffix: "_nofetch", pipeline: try make("solid_fragment", true), constant: SIMD4(1.0 / 255, 0, 0, 0)),
            ]
        } catch {
            return .skip("no pipeline: \(GPU.describe(error))", metrics)
        }
        let target = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        let points = try gpu.buffer(FetchBlendPipeline.quad)
        let instances = try gpu.buffer(FetchBlendPipeline.unmoved)
        let before = RawFacade.counters(of: gpu.device)
        for variant in variants {
            let draw = { (commands: MTLCommandBuffer) in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(variant.pipeline)
                encoder.setVertexBuffer(points, offset: 0, index: 0)
                encoder.setVertexBuffer(instances, offset: 0, index: 1)
                var constant = variant.constant
                encoder.setFragmentBytes(&constant, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
                for _ in 0..<Self.draws {
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            var warm = 0
            while kind == "raw", warm < Self.warmups,
                try await gpu.run(tolerateError: true, draw).path.hasPrefix("m2v:state")
            {
                warm += 1
                try await Task.sleep(nanoseconds: 50_000_000)
            }
            var timings: [GPU.Timing] = []
            for _ in 0..<Self.runs { timings.append(try await gpu.run(draw)) }
            guard timings.allSatisfy(\.gpuTimed) else { return .skip("no GPU timestamps", metrics) }
            let perDraw = timings.map { $0.gpuMilliseconds * 1000 / Double(Self.draws) }
            metrics.set("us_per_draw" + variant.suffix, Stats.percentile(perDraw, 0.5), digits: 1)
            metrics.set("path" + variant.suffix, Set(timings.map(\.path)).sorted().joined(separator: ","))
            if warm > 0 { metrics.set("warm" + variant.suffix, warm) }
        }
        if kind == "raw" {
            metrics.set("fetch", RawFacade.fetchRoutes(before: before, after: RawFacade.counters(of: gpu.device)))
        }
        return .pass(metrics, proof: gpu.proof)
    }
}
