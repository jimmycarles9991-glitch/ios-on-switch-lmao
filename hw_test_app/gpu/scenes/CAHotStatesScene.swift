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
/// - ilk_<label> (raw): the timed runs' draws that took the pixel interlock. Such a state is timed again as the same
///   pixels in eight disjoint bands, no fragment waiting on another (apart_us_<label>, GPU time per full screen), and
///   as the pass with no draw (pass_us_<label>, its clears and setup), which InterlockBound takes out of both.
/// - gpu_clock_<label>=host (raw, debug builds): a timed run's GPU time (the interlock's bands and pass included) was the
///   host's window, for want of a GPU stamp.
/// On the raw facade a state the pack lacks is drawn by m2v until the runtime makes its record: the scene waits for it
/// (warm_<label> runs) and leaves out a state that never reaches the raw path, naming its path.
/// On the process's default device the wide blur (narrow_blur_27) also draws `dumpBuffers` untimed command buffers
/// back to back, and a debug kernel dumps the GPU's registers into its log while the first runs (gpu/RegDump.swift):
/// the regdump metrics.
struct CAHotStatesScene: GPUScene {
    let id = "gpu.ca.hot_states"
    let name = "Core Animation's costliest states: GPU and host time per full-screen draw"

    static let width = 1280
    static let height = 720
    static let draws = InterlockBound.draws
    static let runs = 5
    static let warmups = 20
    static let wideBlur = "narrow_blur_27_frag_lph"
    static let dumpBuffers = 4

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
        let processDefault = gpu.device === MTLCreateSystemDefaultDevice()
        let nanoseconds = kind != "m2v"
        result.metrics.set("gpu_clock", nanoseconds ? "ns" : "ptimer")
        let source = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.shaderRead])
        try await gpu.blitUpload(
            [UploadRect(x: 0, y: 0, width: Self.width, height: Self.height)],
            [Patterns.bytes(Patterns.busy(width: Self.width, height: Self.height, channels: 4), .bgra8Unorm)],
            into: source, texel: 4)
        var timed = 0
        var dumped = Metrics()
        dumped.set("regdump", "hot_states:no_state")
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
            let draw = { (bands: Bool, draws: Int, commands: MTLCommandBuffer) in
                let pass = MTLRenderPassDescriptor()
                for (color, texture) in zip(line.colors, attachments) {
                    pass.colorAttachments[color.index].texture = texture
                    pass.colorAttachments[color.index].loadAction = .clear
                    pass.colorAttachments[color.index].storeAction = color.index == 0 ? .store : .dontCare
                }
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                for band in 0..<draws {
                    let (y0, y1) = bands ? Self.band(band) : (-1, 1)
                    CAState.bind(
                        encoder, vertices: CAState.rectangle(x0: -1, y0: y0, x1: 1, y1: y1, color: SIMD4(1, 1, 1, 1)),
                        source: source)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            let overlapping = { (commands: MTLCommandBuffer) in draw(false, Self.draws, commands) }
            let label = line.shortLabel.lowercased()
            var warm = 0
            if kind == "raw" {
                while warm < Self.warmups, try await gpu.run(tolerateError: true, overlapping).path != "raw" {
                    warm += 1
                    try await Task.sleep(nanoseconds: 50_000_000)
                }
            }
            let before = RawFacade.counters(of: gpu.device)
            var runs: [GPU.Timing] = []
            for _ in 0..<Self.runs {
                runs.append(try await gpu.run(tolerateError: true, overlapping))
            }
            let after = RawFacade.counters(of: gpu.device)
            if processDefault && line.fragment.name == Self.wideBlur {
                let connection = GPUConnection()
                let buffers = try (0..<Self.dumpBuffers).map { _ in try gpu.commit(overlapping) }
                dumped = RegDump.capture(.hotStates, on: connection)
                do {
                    try gpu.finish(buffers)
                } catch {
                    result.notes.append("\(line.shortLabel)'s register dump draw: \(error)")
                }
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
            var timings = runs
            if let interlocked = after["interlock_draws"], interlocked > before["interlock_draws"] ?? 0 {
                result.metrics.set("ilk_" + label, interlocked - (before["interlock_draws"] ?? 0))
                var apart: [Double] = [], pass: [Double] = []
                for _ in 0..<Self.runs {
                    let bands = try await gpu.run(tolerateError: true) { draw(true, Self.draws, $0) }
                    let empty = try await gpu.run(tolerateError: true) { draw(false, 0, $0) }
                    apart.append(bands.gpuMilliseconds * 1000)
                    pass.append(empty.gpuMilliseconds * 1000)
                    timings += [bands, empty]
                }
                result.metrics.set("apart_us_" + label, Stats.percentile(apart, 0.5), digits: 1)
                result.metrics.set("pass_us_" + label, Stats.percentile(pass, 0.5), digits: 1)
            } else if after["interlock_draws"] != nil {
                result.metrics.set("ilk_" + label, 0)
            }
            if timings.contains(where: { $0.gpuClock == "host" }) { result.metrics.set("gpu_clock_" + label, "host") }
        }
        if timed == 0 { result.notes.append("no hot state made from this QuartzCore") }
        if processDefault { result.metrics.merge(dumped) }
        return result
    }

    /// Band `index` of `draws` across the target, in normalized device coordinates.
    static func band(_ index: Int) -> (Float, Float) {
        let height = 2 / Float(draws)
        return (-1 + Float(index) * height, -1 + Float(index + 1) * height)
    }

    func judge(_ metrics: inout Metrics) -> [String] {
        InterlockBound.judge(&metrics)
    }
}
