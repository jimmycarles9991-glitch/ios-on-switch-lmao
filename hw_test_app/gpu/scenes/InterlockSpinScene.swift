import Foundation
import HWTestCore
import Metal

struct InterlockSpinScene: GPUScene {
    let id = "gpu.raw.interlock_spin"
    let name = "Interlock ticket waits: exact overlapping blends and GPU time"

    static let width = 1280, height = 720
    static let layers = [1, 4, 16]
    static let runs = 5
    static let arms = [
        ("plain", "solid_fragment"), ("atomic", "fetch_blend_atomic_fragment"),
        ("default", "fetch_blend_fragment"),
    ]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind != "m2v" else {
            result.notes.append("the raw device and Apple GPU reference only")
            return result
        }
        let format = TexelFormat.rgba16Float
        let scale = FetchBlendState.scale(format)
        let target = try gpu.texture(format.metal, Self.width, Self.height)
        let points = try gpu.buffer(FetchBlendPipeline.quad)
        let tags = (0..<16).map { SIMD4<Float>(0, 0, Float(($0 * 5 + 1) & 3), 0) }
        let instances = try gpu.buffer(tags)
        var values = [FetchBlendState.clear]
        for tag in tags { values.append(FetchBlendState.step(values.last!, UInt32(tag.z))) }
        let pipelines = try Self.arms.map {
            try FetchBlendPipeline.make(gpu, fragment: $0.1, format: format.metal)
        }
        for layers in Self.layers {
            let texel = FetchBlendState.bytes([values[layers]], format)
            let expected = Array(repeating: texel, count: Self.width * Self.height).flatMap { $0 }
            var baseline: [Double] = []
            for (arm, pipeline) in zip(Self.arms, pipelines) {
                let label = "\(arm.0)_\(layers)"
                let draw = { (commands: MTLCommandBuffer) in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = target
                    pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].storeAction = .store
                    let clear = FetchBlendState.clear
                    pass.colorAttachments[0].clearColor = MTLClearColor(
                        red: Double(clear.x) / Double(scale), green: Double(clear.y) / Double(scale),
                        blue: Double(clear.z) / Double(scale), alpha: Double(clear.w) / Double(scale))
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(pipeline)
                    FetchBlendPipeline.bind(encoder, points: points, instances: instances, scale: scale)
                    for layer in 0..<layers {
                        encoder.setVertexBuffer(instances, offset: layer * MemoryLayout<SIMD4<Float>>.stride, index: 1)
                        if arm.0 == "plain" {
                            let delta = values[layer + 1] &- values[layer]
                            var color = SIMD4<Float>((0..<4).map { Float(delta[$0]) / scale })
                            encoder.setFragmentBytes(&color, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
                        }
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    }
                    encoder.endEncoding()
                }
                var warm = try await gpu.run(tolerateError: true, draw)
                for _ in 0..<20 where kind == "raw" && warm.path.hasPrefix("m2v:state") {
                    try await Task.sleep(nanoseconds: 50_000_000)
                    warm = try await gpu.run(tolerateError: true, draw)
                }
                guard !warm.path.hasPrefix("error:"), kind != "raw" || warm.path == "raw" else {
                    result.expect(false, "\(label): \(warm.path)")
                    continue
                }
                let before = RawFacade.counters(of: gpu.device)
                var times: [Double] = []
                for _ in 0..<Self.runs {
                    let timing = try await gpu.run(draw)
                    result.expect(kind != "raw" || timing.path == "raw", "\(label): \(timing.path)")
                    result.expect(timing.gpuTimed && timing.gpuClock != "host", "\(label): no GPU timestamp")
                    times.append(timing.gpuTimed && timing.gpuClock != "host" ? timing.gpuMilliseconds : 0)
                    let bytes = try await gpu.read(target, bytesPerPixel: format.bytesPerTexel)
                    _ = result.compare(
                        label, bytes: bytes, expected: expected, format: format,
                        width: Self.width, height: Self.height, tolerance: 0)
                }
                result.metrics.set("gpu_ms_" + label, Stats.percentile(times, 0.5), digits: 3)
                if kind == "raw" {
                    let after = RawFacade.counters(of: gpu.device)
                    let interlocked = (after["interlock_draws"] ?? 0) - (before["interlock_draws"] ?? 0)
                    result.metrics.set("ilk_" + label, interlocked)
                    result.expect(
                        arm.0 == "plain" ? interlocked == 0 : interlocked >= layers * Self.runs,
                        "\(label): unexpected interlock count \(interlocked)")
                }
                if arm.0 == "plain" {
                    baseline = times
                } else {
                    result.expect(
                        InterlockSpinBound.accepts(times, baseline: baseline),
                        "\(label): a draw exceeded 4x the non-interlocked GPU time or lacked a timestamp")
                }
            }
        }
        return result
    }
}
