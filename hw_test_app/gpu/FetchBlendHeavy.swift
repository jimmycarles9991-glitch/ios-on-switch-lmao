import Foundation
import HWTestCore
import Metal

public enum FetchBlendHeavy {
    public static func run(_ gpu: GPU, draws: Int, runs: Int) async throws -> SceneResult {
        var result = SceneResult()
        let library = try gpu.device.makeLibrary(URL: CAState.library)
        var states = CAHotCorpus.states.compactMap { line -> (String, PipelineLine)? in
            if line.label.contains("TimgBvcm") { return ("timgbvcm", line) }
            if line.label.contains("TgsaA3") { return ("tgsa3", line) }
            return nil
        }
        if ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 27 {
            states.append(("xghfc", CAGenericStateScene.line))
        } else {
            result.metrics.set("tgsa3", "unavailable_on_this_os")
            result.metrics.set("xghfc", "unavailable_on_this_os")
        }
        let source = try gpu.texture(.bgra8Unorm, 64, 64, usage: [.shaderRead])
        try await gpu.blitUpload(
            [UploadRect(x: 0, y: 0, width: 64, height: 64)],
            [Patterns.bytes(Patterns.busy(width: 64, height: 64, channels: 4), .bgra8Unorm)], into: source, texel: 4)
        let raw = RawFacade.kind(of: gpu.device) == "raw"
        for (label, line) in states {
            let pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
            let attachments = try line.colors.map { color in
                try gpu.texture(MTLPixelFormat(rawValue: color.format)!, 1280, 720)
            }
            let draw = { (commands: MTLCommandBuffer) in
                let pass = MTLRenderPassDescriptor()
                for (color, texture) in zip(line.colors, attachments) {
                    pass.colorAttachments[color.index].texture = texture
                    pass.colorAttachments[color.index].loadAction = .clear
                    pass.colorAttachments[color.index].storeAction = .store
                    pass.colorAttachments[color.index].clearColor = MTLClearColor(
                        red: 0.25, green: 0.5, blue: 0.75, alpha: 1)
                }
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                CAState.bind(
                    encoder,
                    vertices: CAState.rectangle(
                        x0: -1, y0: -1, x1: 1, y1: 1,
                        color: SIMD4(1, 1, 1, 1)), source: source)
                let zeros = [UInt8](repeating: 0, count: 256)
                for slot in 4..<8 { encoder.setVertexBytes(zeros, length: zeros.count, index: slot) }
                for slot in 4..<16 { encoder.setFragmentBytes(zeros, length: zeros.count, index: slot) }
                for _ in 0..<draws { encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4) }
                encoder.endEncoding()
            }
            var warm = try await gpu.run(tolerateError: true, draw)
            for _ in 0..<20 where raw && warm.path.hasPrefix("m2v:state") {
                try await Task.sleep(nanoseconds: 50_000_000)
                warm = try await gpu.run(tolerateError: true, draw)
            }
            guard !warm.path.hasPrefix("error:"), !raw || warm.path == "raw" else {
                result.expect(false, "\(label): \(warm.path)")
                continue
            }
            let before = RawFacade.counters(of: gpu.device)
            var times: [Double] = []
            for _ in 0..<runs {
                let timing = try await gpu.run(draw)
                result.expect(!raw || timing.path == "raw", "\(label): \(timing.path)")
                result.expect(timing.gpuTimed && timing.gpuClock != "host", "\(label): no GPU timestamp")
                times.append(timing.gpuMilliseconds)
            }
            result.metrics.set("gpu_ms_" + label, Stats.percentile(times, 0.5), digits: 3)
            result.metrics.set("us_per_draw_" + label, Stats.percentile(times, 0.5) * 1000 / Double(draws), digits: 1)
            if raw {
                result.metrics.set(
                    "fetch_" + label,
                    RawFacade.fetchRoutes(before: before, after: RawFacade.counters(of: gpu.device)))
            }
        }
        return result
    }
}
