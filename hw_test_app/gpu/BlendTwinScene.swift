import Foundation
import HWTestCore
import Metal

/// A Core Animation state that fetches its own colour, drawn by the raw facade with the route a debug build's probe
/// forces (RMDevice switchRawProbeFetchRung:): 4 its storage form (the fetch, and the pipeline's blend where it has one,
/// inside the pixel interlock), 5 its barrier twin a triangle at a time behind the fetch barrier, 6 the twin's whole draw
/// behind one barrier, the ROP blending. Quads are CA's 48-byte vertices at vertex buffer 1, drawn as triangle lists.
public enum BlendTwinScene {
    public static let storage = 4
    public static let twinTriangles = 5
    public static let twinWhole = 6

    /// Two quads whose triangles cover each pixel at most once: CAStateSweep's slanted strip and its rectangle.
    public static func convex() -> [[CAState.Vertex]] {
        [list(CAStateSweep.strip()), list(CAStateSweep.rectangle())]
    }

    /// A strip whose fourth vertex folds back over its first triangle, so the two triangles cover the same pixels.
    public static func folded() -> [[CAState.Vertex]] {
        [
            list([
                CAState.Vertex(-0.9, -0.9, u: 0, v: 1, color: SIMD4(1, 0.5, 0, 0.75)),
                CAState.Vertex(0.9, -0.9, u: 1, v: 1, color: SIMD4(0, 1, 0.25, 1)),
                CAState.Vertex(-0.9, 0.9, u: 0, v: 0, color: SIMD4(0.2, 0.4, 1, 0.5)),
                CAState.Vertex(-0.3, -0.3, u: 1, v: 0, color: SIMD4(1, 1, 1, 0.25)),
            ])
        ]
    }

    /// A quad under a perspective transform whose last vertex is behind the eye (w < 0): its second triangle is clipped
    /// at w = 0 and what is left projects across the first.
    public static func perspectiveCrossing() -> [[CAState.Vertex]] {
        var vertices = [
            CAState.Vertex(-0.9, -0.9, u: 0, v: 1, color: SIMD4(1, 0.5, 0, 0.75)),
            CAState.Vertex(0.9, -0.9, u: 1, v: 1, color: SIMD4(0, 1, 0.25, 1)),
            CAState.Vertex(-0.9, 0.9, u: 0, v: 0, color: SIMD4(0.2, 0.4, 1, 0.5)),
            CAState.Vertex(0.6, 0.6, u: 1, v: 0, color: SIMD4(1, 1, 1, 0.25)),
        ]
        vertices[3].position = SIMD4(0.6, 0.6, 0, -0.4)
        return [list(vertices)]
    }

    /// `count` quads over the whole target, each offset by a pixel or two, in one draw
    public static func overlapping(_ count: Int) -> [CAState.Vertex] {
        (0..<count).flatMap { k -> [CAState.Vertex] in
            let d = Float(k) * 0.01
            return list(CAState.rectangle(x0: -1 + d, y0: -1, x1: 1, y1: 1 - d, color: SIMD4(0.45, 0.1, 0.3, 0.5)))
        }
    }

    /// A strip's four vertices as the triangle list of the same two triangles (0 1 2, 2 1 3)
    static func list(_ strip: [CAState.Vertex]) -> [CAState.Vertex] {
        [strip[0], strip[1], strip[2], strip[2], strip[1], strip[3]].map(CAStateSweep.premultiply)
    }

    static func pass(_ targets: [MTLTexture]) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        for (index, target) in targets.enumerated() {
            pass.colorAttachments[index].texture = target
            pass.colorAttachments[index].loadAction = .clear
            pass.colorAttachments[index].clearColor = CAStateSweep.premultipliedClear
            pass.colorAttachments[index].storeAction = .store
        }
        return pass
    }

    static func targets(_ line: PipelineLine, _ gpu: GPU, _ width: Int, _ height: Int) throws -> [MTLTexture] {
        try line.colors.sorted(by: { $0.index < $1.index }).map { color in
            guard let format = MTLPixelFormat(rawValue: color.format) else {
                throw TestFailure("attachment format \(color.format)")
            }
            return try gpu.texture(format, width, height)
        }
    }

    /// The blended colour's bytes after `draws` (each a triangle list) over CAStateSweep's pattern, and the path drawn
    public static func bytes(
        _ line: PipelineLine, colour: Int, draws: [[CAState.Vertex]], library: MTLLibrary, gpu: GPU
    ) async throws -> (String, [UInt8]) {
        let pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
        let targets = try targets(line, gpu, CAStateSweep.width, CAStateSweep.height)
        let source = try CAStateSweep.patternTexture(gpu, premultiplied: true)
        let sampler = try CAStateSweep.linearSampler(gpu.device)
        let timing = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass(targets))!
            encoder.setRenderPipelineState(pipeline)
            CAStateSweep.bind(encoder, source: source, sampler: sampler)
            for vertices in draws {
                var vertices = vertices
                encoder.setVertexBytes(
                    &vertices, length: MemoryLayout<CAState.Vertex>.stride * vertices.count, index: 1)
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: vertices.count)
            }
            encoder.endEncoding()
        }
        let format = UInt(targets[colour].pixelFormat.rawValue)
        guard let bytes = CAStateSweep.bytesPerTexel(format) else { throw TestFailure("attachment format \(format)") }
        return (timing.path, try await CAStateSweep.readBack(targets[colour], bytesPerPixel: bytes, gpu: gpu))
    }

    /// The GPU's time of one draw of `quads` overlapping full-target quads into a `side`-pixel (or 1280x720) target, the
    /// median of `runs` command buffers of `draws` draws each, from the raw facade's GPU stamps; nil where untimed
    public static func microseconds(
        _ line: PipelineLine, quads: Int, width: Int, height: Int, library: MTLLibrary, gpu: GPU, draws: Int, runs: Int
    ) async throws -> Double? {
        let pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
        let targets = try targets(line, gpu, width, height)
        let source = try CAStateSweep.patternTexture(gpu, premultiplied: true)
        let sampler = try CAStateSweep.linearSampler(gpu.device)
        let quadVertices = overlapping(quads)
        let draw = { (commands: MTLCommandBuffer) in
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass(targets))!
            encoder.setRenderPipelineState(pipeline)
            CAStateSweep.bind(encoder, source: source, sampler: sampler)
            var vertices = quadVertices
            encoder.setVertexBytes(&vertices, length: MemoryLayout<CAState.Vertex>.stride * vertices.count, index: 1)
            for _ in 0..<draws { encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: vertices.count) }
            encoder.endEncoding()
        }
        _ = try await gpu.run(tolerateError: true, draw)
        var timings: [GPU.Timing] = []
        for _ in 0..<runs { timings.append(try await gpu.run(draw)) }
        guard timings.allSatisfy(\.gpuTimed) else { return nil }
        return Stats.percentile(timings.map { $0.gpuMilliseconds * 1000 / Double(draws) }, 0.5)
    }
}
