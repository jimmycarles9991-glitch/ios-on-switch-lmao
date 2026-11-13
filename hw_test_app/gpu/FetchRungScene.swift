import Foundation
import HWTestCore
import Metal

/// A Core Animation state that blends by fetching its colour, drawn by the raw facade with the fetch rung a debug
/// build's probe forces (RMDevice switchRawProbeFetchRung:): 4 its storage form (the fetch and the blend inside the
/// pixel interlock), 2 or 3 its fetch-free form (the ROP's blend or its iterated blend). Each draw is CAStateSweep's
/// with its colours premultiplied, as Core Animation's always are (every attachment cleared, a slanted strip and a
/// rectangle over the pattern) so the two routes' bytes compare, and
/// a full-screen cover, `draws` of them in one command buffer, gives each route's GPU time.
public enum FetchRungScene {
    public static let width = 1280
    public static let height = 720

    /// The facade takes the forced rung for the closure's command buffers; false where it has no probe channel.
    public static func forcing<T>(_ rung: Int, _ device: MTLDevice, _ body: () async throws -> T) async rethrows -> T? {
        let force = NSSelectorFromString("switchRawProbeFetchRung:")
        guard let facade = device as? NSObject, facade.responds(to: force) else { return nil }
        _ = facade.perform(force, with: NSNumber(value: rung))
        defer { _ = facade.perform(force, with: NSNumber(value: 0)) }
        return try await body()
    }

    /// The first attachment's bytes after CAStateSweep's two draws, and the path the facade drew them on.
    public static func bytes(_ line: PipelineLine, library: MTLLibrary, gpu: GPU) async throws -> (String, [UInt8]) {
        let drawn = try await CAStateSweep.draw(line, library: library, gpu: gpu, premultiplied: true)
        return (drawn.path, drawn.attachments.first ?? [])
    }

    /// The GPU's time of one full-screen draw of the state, the median of `runs` command buffers of `draws` each.
    public static func microseconds(
        _ line: PipelineLine, library: MTLLibrary, gpu: GPU, draws: Int, runs: Int
    ) async throws -> (Double, String) {
        let pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
        var targets: [MTLTexture] = []
        for color in line.colors.sorted(by: { $0.index < $1.index }) {
            guard let format = MTLPixelFormat(rawValue: color.format) else {
                throw TestFailure("attachment format \(color.format)")
            }
            targets.append(try gpu.texture(format, width, height))
        }
        let source = try CAStateSweep.patternTexture(gpu, premultiplied: true)
        let sampler = try CAStateSweep.linearSampler(gpu.device)
        let cover = CAState.rectangle(x0: -1, y0: -1, x1: 1, y1: 1, color: SIMD4(0.45, 0.1, 0.3, 0.5))
        let draw = { (commands: MTLCommandBuffer) in
            let pass = MTLRenderPassDescriptor()
            for (index, target) in targets.enumerated() {
                pass.colorAttachments[index].texture = target
                pass.colorAttachments[index].loadAction = .clear
                pass.colorAttachments[index].clearColor = CAStateSweep.premultipliedClear
                pass.colorAttachments[index].storeAction = .store
            }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            CAStateSweep.bind(encoder, source: source, sampler: sampler)
            var vertices = cover
            encoder.setVertexBytes(&vertices, length: MemoryLayout<CAState.Vertex>.stride * vertices.count, index: 1)
            for _ in 0..<draws { encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4) }
            encoder.endEncoding()
        }
        _ = try await gpu.run(tolerateError: true, draw)
        var timings: [GPU.Timing] = []
        for _ in 0..<runs { timings.append(try await gpu.run(draw)) }
        guard timings.allSatisfy(\.gpuTimed) else { return (0, "untimed") }
        let perDraw = timings.map { $0.gpuMilliseconds * 1000 / Double(draws) }
        return (Stats.percentile(perDraw, 0.5), Set(timings.map(\.path)).sorted().joined(separator: ","))
    }

    /// The largest difference in codes between two images of one format's bytes, and how many bytes differ.
    public static func difference(_ a: [UInt8], _ b: [UInt8]) -> (worst: Int, differing: Int) {
        guard a.count == b.count else { return (Int.max, max(a.count, b.count)) }
        var worst = 0
        var differing = 0
        for (x, y) in zip(a, b) where x != y {
            worst = max(worst, abs(Int(x) - Int(y)))
            differing += 1
        }
        return (worst, differing)
    }
}
