import Foundation
import HWTestCore
import Metal

/// fetch_blend_vertex's pipelines (FramebufferFetch.metal): a float4 vertex attribute (clip x, y, tag), an instance
/// offset table at buffer 1 and One/One blending on the attachment the fragment writes.
public enum FetchBlendPipeline {
    /// The target as one strip of two triangles, as Core Animation draws a layer.
    public static let quad: [SIMD4<Float>] = [
        SIMD4(-1, -1, 0, 0), SIMD4(1, -1, 0, 0), SIMD4(-1, 1, 0, 0), SIMD4(1, 1, 0, 0),
    ]
    public static let unmoved: [SIMD4<Float>] = [SIMD4(0, 0, 0, 0)]

    public static func make(
        _ gpu: GPU, fragment: String, format: MTLPixelFormat, blend: Bool = true, attachments: Int = 1, written: Int = 0
    ) throws -> MTLRenderPipelineState {
        try gpu.render(vertex: "fetch_blend_vertex", fragment: fragment, format: format) { descriptor in
            let vertices = MTLVertexDescriptor()
            vertices.attributes[0].format = .float4
            vertices.attributes[0].bufferIndex = 0
            vertices.layouts[0].stride = MemoryLayout<SIMD4<Float>>.stride
            descriptor.vertexDescriptor = vertices
            for index in 0..<attachments {
                descriptor.colorAttachments[index].pixelFormat = format
                descriptor.colorAttachments[index].writeMask = index == written ? .all : []
            }
            let target = descriptor.colorAttachments[written]!
            target.isBlendingEnabled = blend
            target.sourceRGBBlendFactor = .one
            target.destinationRGBBlendFactor = .one
            target.sourceAlphaBlendFactor = .one
            target.destinationAlphaBlendFactor = .one
        }
    }

    public static func bind(_ encoder: MTLRenderCommandEncoder, points: MTLBuffer, instances: MTLBuffer, scale: Float) {
        var scale = scale
        encoder.setVertexBuffer(points, offset: 0, index: 0)
        encoder.setVertexBuffer(instances, offset: 0, index: 1)
        encoder.setFragmentBytes(&scale, length: 4, index: 0)
    }
}

/// One step of every pixel of a rectangle, as the CPU replays a draw: a pixel-aligned rectangle's two triangles, or a
/// triangle over the whole target.
struct FetchBlendLayer: Sendable {
    let x0: Int, y0: Int, x1: Int, y1: Int
    let tag: UInt32

    static func whole(_ tag: UInt32) -> FetchBlendLayer { FetchBlendLayer(x0: 0, y0: 0, x1: .max, y1: .max, tag: tag) }

    func moved(_ shift: FetchBlendShift) -> FetchBlendLayer {
        guard x1 != .max else { return .whole(tag ^ shift.tag) }
        return FetchBlendLayer(
            x0: x0 + shift.dx, y0: y0 + shift.dy, x1: x1 + shift.dx, y1: y1 + shift.dy, tag: tag ^ shift.tag)
    }
}

/// An instance's move in pixels and the tag it folds in.
struct FetchBlendShift: Sendable {
    let dx: Int, dy: Int
    let tag: UInt32

    static let none = FetchBlendShift(dx: 0, dy: 0, tag: 0)

    func clip(size: Int) -> SIMD4<Float> {
        SIMD4(Float(dx * 2) / Float(size), -Float(dy * 2) / Float(size), Float(tag), 0)
    }
}

enum FetchBlendPiece: Sendable {
    case whole(UInt32)
    case rect(Int, Int, Int, Int, UInt32)
}

enum FetchBlendStrip: Sendable {
    /// Triangles over the whole target, each its own step: vertex j's tag is (j & 1) | bits, the same at j and j + 2, so
    /// a triangle's step is the same whichever of its vertices provokes.
    case folded(triangles: Int, bits: UInt32)
    /// Rectangles side by side between the x edges, together one step.
    case band(xs: [Int], y0: Int, y1: Int, tag: UInt32)
    /// Fewer than three vertices before the restart: no triangle.
    case stub(vertices: Int)
}

/// fetch_blend_vertex's vertices, strip indices with restarts (UInt32.max) and the layers the draw makes, in order.
struct FetchBlendMesh: Sendable {
    static let size = 64
    /// Far past the target on every side, so an instance's move still leaves all of it covered.
    static let whole: [SIMD2<Float>] = [SIMD2(-2, -2), SIMD2(6, -2), SIMD2(-2, 6)]

    var points: [SIMD4<Float>] = []
    var indices: [UInt32] = []
    var layers: [FetchBlendLayer] = []

    static func clip(_ x: Int, _ y: Int) -> SIMD2<Float> {
        SIMD2(Float(x) / Float(size) * 2 - 1, 1 - Float(y) / Float(size) * 2)
    }

    var indices16: [UInt16] {
        indices.map { $0 == .max ? .max : UInt16($0) }
    }

    mutating func point(_ position: SIMD2<Float>, _ tag: UInt32) {
        points.append(SIMD4(position.x, position.y, Float(tag), 0))
    }

    /// `step` is the tag the fragment takes the piece's step with, where it is not the vertices' own.
    mutating func add(_ piece: FetchBlendPiece, step: UInt32? = nil) {
        switch piece {
        case .whole(let tag):
            for corner in Self.whole { point(corner, tag) }
            layers.append(.whole(step ?? tag))
        case .rect(let x0, let y0, let x1, let y1, let tag):
            let (a, b, c, d) = (Self.clip(x0, y0), Self.clip(x1, y0), Self.clip(x0, y1), Self.clip(x1, y1))
            for corner in [a, b, c, b, d, c] { point(corner, tag) }
            layers.append(FetchBlendLayer(x0: x0, y0: y0, x1: x1, y1: y1, tag: step ?? tag))
        }
    }

    static func list(_ pieces: [FetchBlendPiece]) -> FetchBlendMesh {
        var mesh = FetchBlendMesh()
        for piece in pieces { mesh.add(piece) }
        return mesh
    }

    /// Strips, each followed by a restart, their vertices from `base` on.
    static func strips(_ strips: [FetchBlendStrip], base: Int = 0) -> FetchBlendMesh {
        var mesh = FetchBlendMesh()
        mesh.points = Array(repeating: SIMD4<Float>(0, 0, 0, 0), count: base)
        for strip in strips {
            let first = mesh.points.count
            switch strip {
            case .folded(let triangles, let bits):
                for j in 0..<(triangles + 2) { mesh.point(whole[j % 3], UInt32(j & 1) | bits) }
                mesh.layers += (0..<triangles).map { .whole(UInt32($0 & 1) | bits) }
            case .band(let xs, let y0, let y1, let tag):
                for x in xs {
                    mesh.point(clip(x, y0), tag)
                    mesh.point(clip(x, y1), tag)
                }
                mesh.layers.append(FetchBlendLayer(x0: xs.first!, y0: y0, x1: xs.last!, y1: y1, tag: tag))
            case .stub(let vertices):
                for _ in 0..<vertices { mesh.point(whole[0], 0) }
            }
            mesh.indices += (first..<mesh.points.count).map(UInt32.init) + [.max]
        }
        return mesh
    }

    /// Rectangles, each its own indexed strip of four vertices (16-bit indices, no restart), as RenderBox draws a quad
    static func quads(_ count: Int) -> FetchBlendMesh {
        var mesh = FetchBlendMesh()
        for q in 0..<count {
            let (x0, y0) = ((q * 13) % 40, (q * 23) % 40)
            let (x1, y1) = (min(size, x0 + 10 + (q * 7) % 24), min(size, y0 + 12 + (q * 5) % 20))
            let tag = UInt32((q * 3 + q / 4) & 3)
            let first = UInt32(mesh.points.count)
            for corner in [clip(x0, y0), clip(x1, y0), clip(x0, y1), clip(x1, y1)] { mesh.point(corner, tag) }
            mesh.indices += (first..<first + 4).map { $0 }
            mesh.layers.append(FetchBlendLayer(x0: x0, y0: y0, x1: x1, y1: y1, tag: tag))
        }
        return mesh
    }

    /// `triangles` triangles of whole-target triangles and a rectangle every `rectEvery` pieces, their tags and places
    /// from `seed`.
    static func pieces(triangles: Int, seed: Int, rectEvery: Int = 5) -> [FetchBlendPiece] {
        var pieces: [FetchBlendPiece] = []
        var (count, i) = (0, seed)
        while count < triangles {
            let tag = UInt32((i * 7 + i / 3) & 3)
            if i % rectEvery == 2 && count + 2 <= triangles {
                let (x0, y0) = ((i * 13) % 40, (i * 29) % 40)
                pieces.append(.rect(x0, y0, min(size, x0 + 8 + (i * 5) % 24), min(size, y0 + 8 + (i * 11) % 24), tag))
                count += 2
            } else {
                pieces.append(.whole(tag))
                count += 1
            }
            i += 1
        }
        return pieces
    }

    /// `count` steps of two triangles each (a rectangle, or two whole-target triangles), the vertices' tags 0: pair m's
    /// step is fetch_blend_primitive_fragment's primitive_id >> 1.
    static func primitivePairs(_ count: Int) -> FetchBlendMesh {
        var mesh = FetchBlendMesh()
        for m in 0..<count {
            let step = UInt32(m)
            if m % 3 == 1 {
                let (x0, y0) = ((m * 11) % 36, (m * 17) % 36)
                mesh.add(.rect(x0, y0, x0 + 12 + m % 16, y0 + 10 + (m * 7) % 18, 0), step: step)
            } else {
                mesh.add(.whole(0), step: step)
                mesh.add(.whole(0), step: step)
            }
        }
        return mesh
    }
}

/// A pixel's state in steps of 1/scale per channel: fetch_blend_step (FramebufferFetch.metal) on the CPU.
enum FetchBlendState {
    static let clear = SIMD4<Int>(2, 0, 1, 3)

    static func scale(_ format: TexelFormat) -> Float { format.isFloat ? 256 : format == .bgr10a2Unorm ? 1023 : 255 }

    /// What the pixel's next fetch reads back of state `n` once stored in `format`: itself, but a two-bit alpha's
    static func stored(_ n: SIMD4<Int>, _ format: TexelFormat) -> SIMD4<Int> {
        let scale = scale(format)
        let values = format.decode(format.encode((0..<4).map { Float(n[$0]) / scale })[...])
        return SIMD4((0..<4).map { Int((values[$0] * scale).rounded()) })
    }

    static func step(_ n: SIMD4<Int>, _ tag: UInt32) -> SIMD4<Int> {
        let full = n.x < 255 ? 0 : 1
        let mixed = (n.x + n.y) ^ Int(tag)
        return n &+ SIMD4(1 - full, full, mixed & 1, (mixed >> 1) & 1)
    }

    static func replay(_ layers: [FetchBlendLayer], size: Int, format: TexelFormat) -> [SIMD4<Int>] {
        var pixels = [SIMD4<Int>](repeating: stored(clear, format), count: size * size)
        for layer in layers {
            let (xs, ys) = ((max(0, layer.x0), min(size, layer.x1)), (max(0, layer.y0), min(size, layer.y1)))
            guard xs.0 < xs.1, ys.0 < ys.1 else { continue }
            for y in ys.0..<ys.1 {
                for x in xs.0..<xs.1 { pixels[y * size + x] = stored(step(pixels[y * size + x], layer.tag), format) }
            }
        }
        return pixels
    }

    static func bytes(_ pixels: [SIMD4<Int>], _ format: TexelFormat) -> [UInt8] {
        let scale = scale(format)
        return pixels.flatMap { n in format.encode((0..<4).map { Float(n[$0]) / scale }) }
    }

    static func units(_ bytes: [UInt8], _ format: TexelFormat, pixel: Int) -> SIMD4<Int> {
        let at = pixel * format.bytesPerTexel
        let values = format.decode(bytes[at..<(at + format.bytesPerTexel)])
        return SIMD4((0..<4).map { Int((values[$0] * scale(format)).rounded()) })
    }

    static func shown(_ n: SIMD4<Int>) -> String { "\(n.x)/\(n.y)/\(n.z)/\(n.w)" }
}
