import Foundation
import HWTestCore
import Metal

/// The time of Core Animation's costliest states (CAHotCorpus) at the console's resolution: each state draws `draws`
/// full-screen rectangles into a 1280x720 target of its first colour's format, its other colours memoryless, sampling
/// a BGRA8 picture with zero uniforms (as the raw pack's probe binds them). Medians of `runs` command buffers, the
/// label lowercased:
/// - us_<label>: GPU time per draw as the device answers GPUStartTime/GPUEndTime, in the clock gpu_clock names. The raw
///   path's are nanoseconds; m2v's are PTIMER units read as nanoseconds (NVK's timestampPeriod is 1, and this PTIMER
///   counts 0.6144 ns a nanosecond), so they show the work 1.6276 times faster than it ran.
/// - wall_us_<label>: per draw from the commit's return to the completed handler, on the host's clock on both devices.
/// - cpu_us_<label>: per command buffer, the encode and the commit call (raw encodes and submits inside commit).
/// - stall_us_<label>: per command buffer, the host's time after the commit that the GPU time does not account for (the
///   wait for the GPU and the completion's latency), where the GPU time is in nanoseconds.
/// On the raw facade a state the pack lacks is drawn by m2v until the runtime makes its record: the scene waits for it
/// (warm_<label> runs) and leaves out a state that never reaches the raw path, naming its path.
struct CAHotStatesScene: GPUScene {
    let id = "gpu.ca.hot_states"
    let name = "Core Animation's costliest states: GPU and host time per full-screen draw"

    static let width = 1280
    static let height = 720
    static let draws = 8
    static let runs = 5
    static let warmups = 20

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let library: MTLLibrary
        do {
            library = try gpu.device.makeLibrary(URL: CAState.library)
        } catch {
            result.notes.append("no QuartzCore library here (\(GPU.describe(error)))")
            return result
        }
        let kind = RawFacade.kind(of: gpu.device)
        let nanoseconds = kind != "m2v"
        result.metrics.set("gpu_clock", nanoseconds ? "ns" : "ptimer")
        let source = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.shaderRead])
        try await gpu.blitUpload(
            [UploadRect(x: 0, y: 0, width: Self.width, height: Self.height)],
            [Patterns.bytes(Patterns.busy(width: Self.width, height: Self.height, channels: 4), .bgra8Unorm)],
            into: source, texel: 4)
        var timed = 0
        for line in CAHotCorpus.states {
            let pipeline: MTLRenderPipelineState
            do {
                pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
            } catch {
                continue
            }
            let attachments = try line.colors.map { color -> MTLTexture in
                let format = MTLPixelFormat(rawValue: color.format) ?? .bgra8Unorm
                return try color.index == 0
                    ? gpu.texture(format, Self.width, Self.height)
                    : gpu.texture(format, Self.width, Self.height, usage: [.renderTarget], storage: .memoryless)
            }
            let draw = { (commands: MTLCommandBuffer) in
                let pass = MTLRenderPassDescriptor()
                for (color, texture) in zip(line.colors, attachments) {
                    pass.colorAttachments[color.index].texture = texture
                    pass.colorAttachments[color.index].loadAction = .clear
                    pass.colorAttachments[color.index].storeAction = color.index == 0 ? .store : .dontCare
                }
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                CAState.bind(
                    encoder, vertices: CAState.rectangle(x0: -1, y0: -1, x1: 1, y1: 1, color: SIMD4(1, 1, 1, 1)),
                    source: source)
                for _ in 0..<Self.draws {
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            let label = line.shortLabel.lowercased()
            var warm = 0
            if kind == "raw" {
                while warm < Self.warmups, try await gpu.run(tolerateError: true, draw).path != "raw" {
                    warm += 1
                    try await Task.sleep(nanoseconds: 50_000_000)
                }
            }
            var runs: [GPU.Timing] = []
            for _ in 0..<Self.runs {
                runs.append(try await gpu.run(tolerateError: true, draw))
            }
            let paths = Set(runs.map(\.path))
            let named = paths.sorted().joined(separator: ",")
            result.expect(!paths.contains { $0.hasPrefix("error:") }, "\(line.shortLabel): \(named)")
            if kind == "raw" && paths != ["raw"] {
                result.notes.append("\(line.shortLabel) left out: drawn on \(named) after \(warm) runs")
                continue
            }
            timed += 1
            let perDraw = 1000 / Double(Self.draws)
            let afterCommit = runs.map { $0.wallMilliseconds - $0.commitMilliseconds }
            let gpuPerDraw = runs.map { $0.gpuMilliseconds * perDraw }
            result.metrics.set("us_" + label, Stats.percentile(gpuPerDraw, 0.5), digits: 1)
            result.metrics.set("wall_us_" + label, Stats.percentile(afterCommit.map { $0 * perDraw }, 0.5), digits: 1)
            result.metrics.set(
                "cpu_us_" + label,
                Stats.percentile(runs.map { ($0.encodeMilliseconds + $0.commitMilliseconds) * 1000 }, 0.5), digits: 0)
            if nanoseconds {
                let stalls = zip(afterCommit, runs).map { ($0 - $1.gpuMilliseconds) * 1000 }
                result.metrics.set("stall_us_" + label, Stats.percentile(stalls, 0.5), digits: 0)
            }
            if warm > 0 { result.metrics.set("warm_" + label, warm) }
        }
        if timed == 0 { result.notes.append("no hot state made from this QuartzCore") }
        return result
    }
}
