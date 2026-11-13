import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// index.past_limit (kits/SwitchGM20B caps row 3D-B11, raw_gpu.cpp drawIndexed): what GM20B's index fetch reads past
/// SET_INDEX_BUFFER_C/D's limit. Core Animation's fill draws a strip of five 16-bit indices from a buffer holding
/// [1, 2, 3, 4, 5, 5, 5, 5] under the probe form index=8, so the fetch ends after [1, 2, 3, 4]. The strip's first two
/// triangles cover the bottom-left quarter (`quarter`: the draw ran); its third, (3, 4, fifth index), covers pixel `p0`
/// where the fifth index reads 0, Metal's answer past a buffer's end, and pixel `p5` where it reads the 5 in memory past
/// the limit; neither where the fetch stops at the limit. PASS: the caps row may go to chip grade and the runtime binds
/// an index buffer past its end with no padded copy.
enum X1IndexLimitProbe {
    static let probe = X1Probe(
        id: "gpu.x1.index_past_limit", name: "The index fetch reads index 0 past its limit", probe: "index.past_limit",
        risky: true, timeout: 60, body: run)

    static let size = 64
    static let quarter = (16, 48), p0 = (48, 12), p5 = (44, 51)

    static func run(_ gpu: GPU) async throws -> ProbeFinding {
        let journal = ProbeJournal(probe: "gpu.x1.index_past_limit")
        journal.starting("fill strip of five indices, the fetch ending after four")
        let library = try gpu.device.makeLibrary(URL: CAState.library)
        guard let line = CACorpus.state("PBGRAXm_A2Xhf") else { throw TestFailure("CACorpus has no PBGRAXm_A2Xhf") }
        let pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
        let target = try gpu.texture(.bgra8Unorm, size, size)
        let scratch = try gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget], storage: .memoryless)
        let white = SIMD4<Float16>(1, 1, 1, 1)
        let corners: [(Float, Float)] = [(1, 1), (-1, -1), (0, -1), (-1, 0), (0, 0), (1, -1)]
        let vertices = corners.map { CAState.Vertex($0.0, $0.1, u: 0, v: 0, color: white) }
        let indices = try gpu.buffer([UInt16]([1, 2, 3, 4, 5, 5, 5, 5]))
        let batch = try await gpu.run(forms: "index=8") { commands in
            let descriptor = MTLRenderPassDescriptor()
            descriptor.colorAttachments[0].texture = target
            descriptor.colorAttachments[0].loadAction = .clear
            descriptor.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            descriptor.colorAttachments[0].storeAction = .store
            descriptor.colorAttachments[1].texture = scratch
            descriptor.colorAttachments[1].loadAction = .clear
            descriptor.colorAttachments[1].storeAction = .dontCare
            let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)!
            encoder.setRenderPipelineState(pipeline)
            CAState.bind(encoder, vertices: vertices, source: nil)
            encoder.drawIndexedPrimitives(
                type: .triangleStrip, indexCount: 5, indexType: .uint16, indexBuffer: indices, indexBufferOffset: 0)
            encoder.endEncoding()
        }
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let drawn = { (at: (Int, Int)) in pixels.pixel(at.0, at.1, width: size).contains { $0 != 0 } }
        var metrics = Metrics()
        metrics.set("path", batch.path)
        metrics.set("index_limits", batch.taken["index_limits"] ?? 0)
        metrics.set("quarter", drawn(quarter))
        metrics.set("p0", drawn(p0))
        metrics.set("p5", drawn(p5))
        if batch.path != "raw" || (batch.taken["index_limits"] ?? 0) == 0 {
            return ProbeFinding(.inconclusive, metrics, note: "the draw did not take the index=8 form on the raw path")
        }
        if !drawn(quarter) {
            return ProbeFinding(.inconclusive, metrics, note: "the strip's first triangles drew nothing")
        }
        switch (drawn(p0), drawn(p5)) {
        case (true, false):
            metrics.set("past_limit", "zero")
            return ProbeFinding(.pass, metrics)
        case (false, true):
            metrics.set("past_limit", "memory")
            return ProbeFinding(.fail, metrics, note: "the fetch read the index in memory past its limit")
        case (false, false):
            metrics.set("past_limit", "stops")
            return ProbeFinding(
                .fail, metrics, note: "the fetch stopped at its limit: the strip's last triangle dropped")
        case (true, true):
            metrics.set("past_limit", "both")
            return ProbeFinding(.inconclusive, metrics, note: "both probe pixels drawn")
        }
    }
}
