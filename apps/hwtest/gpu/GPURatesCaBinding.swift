import Foundation
import HWTestCore
import Metal
import simd

/// gpu.perf.ca_binding: the render server's Shared buffers bind L2-bypassing (VOL=1), apps' L2-cached (ca3396c7e). This
/// replays Core Animation's costliest states (`states`, HWTestScenes' CAHotCorpus) as the render server feeds a frame: per command buffer the
/// CPU writes each draw's vertices, transform and uniforms into a Shared ring, and `passes` passes of `draws` small
/// rectangles bind them at offsets. Each state runs with its ring made L2-cached (`cached`) and made L2-bypassing
/// (`volatile`, switchMetalSharedBypassesL2:), each ring's binding read back (switchMetalProbeBufferFacts:) before it is
/// timed. Per state label, medians of `runs` command buffers: `ca_<label>_<binding>_us` GPU time, `_wall_us` commit to
/// completion, `_stall_us` the wall time the GPU time does not account for, where the kernel's host-written invalidate
/// and External flush sit. It measures the binding in an app process: the render server's own priority and present path
/// are not in it.
extension GPURates {
    public static func caBinding(_ gpu: GPU, _ s: Sizes = .device, states: [PipelineLine]) async throws -> SceneResult {
        let b = GPURates(gpu, s)
        var notes: [String] = []
        let library: MTLLibrary
        do {
            library = try gpu.device.makeLibrary(URL: CAState.library)
        } catch {
            var r = SceneResult()
            r.notes.append("no QuartzCore library here (\(GPU.describe(error)))")
            return r
        }
        let bindings = b.has("switchMetalSharedBypassesL2:") ? ["cached", "volatile"] : ["cached"]
        if bindings.count == 1 { notes.append("the device has no switchMetalSharedBypassesL2: probe: no volatile arm") }
        let source = try gpu.texture(.bgra8Unorm, s.width, s.height, usage: [.shaderRead])
        var timed = 0
        for line in states {
            guard let pipeline = try? CAState.pipeline(line, library: library, device: gpu.device) else { continue }
            let label = line.shortLabel.lowercased()
            for binding in bindings {
                await b.isolated("ca_\(label)_\(binding)") {
                    try await b.caReplay(line, pipeline, source, label, binding)
                }
            }
            timed += 1
        }
        if timed == 0 { notes.append("no hot state made from this QuartzCore") }
        var r = SceneResult()
        r.metrics = b.metrics
        r.notes = notes
        for p in b.problems { r.expect(false, p) }
        return r
    }

    static let caDraws = 32, caPasses = 4, caSlot = 256, caFrames = 3

    var caFrameBytes: Int { Self.caDraws * Self.caPasses * 3 * Self.caSlot + Self.caSlot }

    /// A Shared ring of `caFrames` frames made under `binding`, its route checked
    func caRing(_ binding: String, _ arm: String) throws -> MTLBuffer {
        let bypass = binding == "volatile"
        probe("switchMetalSharedBypassesL2:", bypass ? 1 : 0)
        defer { probe("switchMetalSharedBypassesL2:", 0) }
        guard let ring = gpu.device.makeBuffer(length: caFrameBytes * Self.caFrames, options: .storageModeShared) else {
            throw TestFailure("\(arm): makeBuffer returned nil")
        }
        let facts = Self.fields(ask(gpu.device, "switchMetalProbeBufferFacts:", ring))
        if facts["raw"] == 1 && facts["volatile"] != (bypass ? 1 : 0) {
            problems.append("\(arm): the ring bound volatile=\(facts["volatile"] ?? -1), not the arm's binding")
        }
        metrics.set("\(arm)_route", facts["volatile"].map { $0 == 1 ? "volatile" : "cached" } ?? "-")
        return ring
    }

    /// Frame `frame`'s vertices, transforms and uniforms written into its third of the ring, as CA writes a frame's
    func caWrite(_ ring: MTLBuffer, frame: Int) -> Int {
        let base = (frame % Self.caFrames) * caFrameBytes
        let bytes = ring.contents().advanced(by: base)
        memset(bytes, 0, Self.caSlot)
        var identity = matrix_identity_float4x4
        let columns = 8, rows = Self.caDraws / columns
        let width = 2 / Float(columns), height = 2 / Float(rows)
        for draw in 0 ..< Self.caDraws * Self.caPasses {
            let at = Self.caSlot + draw * 3 * Self.caSlot
            let i = draw % Self.caDraws
            let x0 = -1 + Float(i % columns) * width, y0 = -1 + Float(i / columns) * height
            let colour = SIMD4<Float16>(Float16(Float(i) / Float(Self.caDraws)), 0.5, Float16(Float(frame % 7) / 7), 1)
            var vertices = CAState.rectangle(x0: x0, y0: y0, x1: x0 + width, y1: y0 + height, color: colour)
            memcpy(bytes.advanced(by: at), &vertices, MemoryLayout<CAState.Vertex>.stride * vertices.count)
            memcpy(bytes.advanced(by: at + Self.caSlot), &identity, MemoryLayout<simd_float4x4>.size)
            memset(bytes.advanced(by: at + 2 * Self.caSlot), 0, Self.caSlot)
        }
        return base
    }

    func caReplay(
        _ line: PipelineLine, _ pipeline: MTLRenderPipelineState, _ source: MTLTexture, _ label: String,
        _ binding: String
    ) async throws {
        let arm = "ca_\(label)_\(binding)"
        let ring = try caRing(binding, arm)
        let attachments = try line.colors.map { color -> MTLTexture in
            let format = MTLPixelFormat(rawValue: color.format) ?? .bgra8Unorm
            return try color.index == 0
                ? gpu.texture(format, s.width, s.height)
                : gpu.texture(format, s.width, s.height, usage: [.renderTarget], storage: .memoryless)
        }
        var frame = 0
        let encode = { (c: MTLCommandBuffer) in
            let base = self.caWrite(ring, frame: frame)
            frame += 1
            for p in 0 ..< Self.caPasses {
                let pass = MTLRenderPassDescriptor()
                for (color, texture) in zip(line.colors, attachments) {
                    pass.colorAttachments[color.index].texture = texture
                    pass.colorAttachments[color.index].loadAction = p == 0 || color.index != 0 ? .clear : .load
                    pass.colorAttachments[color.index].storeAction = color.index == 0 ? .store : .dontCare
                }
                let e = c.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(pipeline)
                e.setFragmentTexture(source, index: 0)
                e.setFragmentTexture(source, index: 1)
                for slot in [0, 3] { e.setVertexBuffer(ring, offset: base, index: slot) }
                for i in 0 ..< Self.caDraws {
                    let at = base + Self.caSlot + (p * Self.caDraws + i) * 3 * Self.caSlot
                    e.setVertexBuffer(ring, offset: at, index: 1)
                    e.setVertexBuffer(ring, offset: at + Self.caSlot, index: 2)
                    for slot in 0 ..< 4 { e.setFragmentBuffer(ring, offset: at + 2 * Self.caSlot, index: slot) }
                    e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                e.endEncoding()
            }
        }
        try await gpu.run(encode)
        var runs: [GPU.Timing] = []
        for _ in 0 ..< s.repeats { runs.append(try await gpu.run(tolerateError: true, encode)) }
        let paths = Set(runs.map(\.path))
        let offDriver = GM20BFacade.kind(of: gpu.device) == "gm20b" && paths != ["gm20b"]
        if paths.contains(where: { $0.hasPrefix("error:") }) || offDriver {
            problems.append("\(arm): drawn on \(paths.sorted().joined(separator: ","))")
            return
        }
        let afterCommit = runs.map { $0.wallMilliseconds - $0.commitMilliseconds }
        metrics.set("\(arm)_us", Stats.percentile(runs.map { $0.gpuMilliseconds * 1000 }, 0.5), digits: 1)
        metrics.set("\(arm)_wall_us", Stats.percentile(afterCommit.map { $0 * 1000 }, 0.5), digits: 1)
        let stalls = zip(afterCommit, runs).map { ($0 - $1.gpuMilliseconds) * 1000 }
        metrics.set("\(arm)_stall_us", Stats.percentile(stalls, 0.5), digits: 0)
    }
}
