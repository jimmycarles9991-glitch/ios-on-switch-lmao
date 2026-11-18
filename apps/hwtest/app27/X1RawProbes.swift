import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The probes that program MAXWELL_B methods no open driver runs on GM20B (clb197.h), through RawMethods. Each is its
/// own test, so the run's journal names it before it starts, and each puts back every register it set after its draw.
enum X1RawProbes {
    static let setIteratedBlendOptimization: UInt32 = 0x02f4
    static let setIteratedBlend: UInt32 = 0x0dd0
    static let setIteratedBlendPass: UInt32 = 0x0dd4
    static let loadIteratedBlendPointer: UInt32 = 0x0df0
    static let loadIteratedBlendInstruction: UInt32 = 0x0df4
    static let setViewportCoordinateSwizzle0: UInt32 = 0x0a18

    static func pass(_ target: MTLTexture, load: MTLLoadAction) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = load
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        return pass
    }

    static func sourceOver(_ descriptor: MTLRenderPipelineDescriptor) {
        let attachment = descriptor.colorAttachments[0]!
        attachment.isBlendingEnabled = true
        attachment.sourceRGBBlendFactor = .one
        attachment.sourceAlphaBlendFactor = .one
        attachment.destinationRGBBlendFactor = .oneMinusSourceAlpha
        attachment.destinationAlphaBlendFactor = .oneMinusSourceAlpha
    }

    /// METALMAP d13: source-over of (0, 0, 0, 0) onto RGBA16F holding -0, NaN, infinities and denormals, and onto
    /// BGRA8 bytes, with SET_ITERATED_BLEND_OPTIMIZATION NOOP_SOURCE_RGBA_0000 (NVK already allows float pixel kills).
    /// The claim: with it, no destination bit changes.
    static let blendKills = X1Probe(
        id: "gpu.x1.blend_kills", name: "Iterated-blend optimization: a zero source-over kills the pixel, bits kept",
        probe: "d13", risky: true
    ) { gpu in
        let special: [UInt16] = [
            0x8000, 0x7e00, 0xfe00, 0x7c00, 0xfc00, 0x0001, 0x8001, 0x03ff, 0x3c00, 0xbc00, 0x7bff, 0x0000, 0x3555,
            0x0400, 0x8400, 0x7d00,
        ]
        let halves = (0..<64).map { special[$0 % special.count] }
        let unorm = (0..<64).map { UInt8(($0 * 53 + 7) & 255) }
        var wrong: [String: Int] = [:]
        var metrics = Metrics()
        var paths: [String] = []
        var emitted = 0
        for (mode, words) in [("plain", [(UInt32, UInt32)]()), ("noop", [(setIteratedBlendOptimization, UInt32(1))])] {
            for half in [true, false] {
                let format: MTLPixelFormat = half ? .rgba16Float : .bgra8Unorm
                let target = try gpu.texture(format, 16, 1, usage: [.renderTarget, .shaderRead])
                let initial = half ? halves.flatMap { [UInt8($0 & 0xff), UInt8($0 >> 8)] } : unorm
                try await gpu.upload(initial, to: target, bytesPerRow: initial.count)
                let state = try gpu.render(fragment: "exact_fragment", format: format, configure: sourceOver)
                let draw = { (commands: MTLCommandBuffer) in
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass(target, load: .load))!
                    encoder.setRenderPipelineState(state)
                    var corners = GPU.fullScreen
                    var zero = SIMD4<Float>(0, 0, 0, 0)
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    encoder.setFragmentBytes(&zero, length: 16, index: 0)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    encoder.endEncoding()
                }
                if words.isEmpty {
                    paths.append(try await gpu.run(draw).path)
                } else {
                    let drawn = try await RawMethods.draw(
                        gpu, before: words, after: [(setIteratedBlendOptimization, 0)], draw)
                    paths.append(drawn.path)
                    emitted += drawn.emitted
                }
                let after = try await gpu.read(target, bytesPerPixel: half ? 8 : 4)
                let changed = zip(after, initial).filter { $0 != $1 }.count
                let name = "\(mode)_\(half ? "f16" : "unorm")_changed_bytes"
                if mode == "noop" { wrong[name] = changed } else { metrics.set(name, changed) }
                if half && mode == "plain" {
                    metrics.set("plain_negzero_bits", String(UInt16(after[0]) | UInt16(after[1]) << 8, radix: 16))
                }
            }
        }
        return ProbeFinding.counted(wrong, paths: paths, metrics: metrics).emitting(emitted)
    }

    /// The six vertices of each rectangle [x0, x1) x [y0, y1) of a size x size target, in clip space.
    static func rectangles(_ rects: [(Int, Int, Int, Int)], size: Int) -> [SIMD4<Float>] {
        rects.flatMap { r -> [SIMD4<Float>] in
            let c = GPU.pixelQuad(r.0, r.1, r.2, r.3, width: size, height: size)
            return [c[0], c[1], c[2], c[1], c[3], c[2]]
        }
    }

    /// METALMAP d26 / F10: SET_VIEWPORT_COORDINATE_SWIZZLE on viewport 0 with x and y exchanged. Two rectangles of
    /// their own colours drawn plainly and swizzled; the swizzled picture is matched against the plain one's eight
    /// flips and turns. The claim: the swizzle moves the picture by one of the two that exchange x and y.
    static let viewportSwizzle = X1Probe(
        id: "gpu.x1.viewport_swizzle", name: "Viewport coordinate swizzle: x and y exchanged in hardware",
        probe: "d26/F10", risky: true
    ) { gpu in
        let size = 64
        let corners = try gpu.buffer(rectangles([(0, 0, 32, 16), (0, 16, 16, 48)], size: size))
        let colors = try gpu.buffer([SIMD4<Float>(1, 0, 0, 1), SIMD4<Float>(0, 1, 0, 1)])
        let state = try gpu.render(vertex: "x1_colored_vertex", fragment: "x1_colored_fragment", format: .bgra8Unorm)
        var pictures: [[UInt8]] = []
        var paths: [String] = []
        var emitted = 0
        for swizzled in [false, true] {
            let target = try gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget, .shaderRead])
            let draw = { (commands: MTLCommandBuffer) in
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass(target, load: .clear))!
                encoder.setRenderPipelineState(state)
                encoder.setVertexBuffer(corners, offset: 0, index: 0)
                encoder.setVertexBuffer(colors, offset: 0, index: 1)
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 12)
                encoder.endEncoding()
            }
            if swizzled {
                let drawn = try await RawMethods.draw(
                    gpu, before: [(setViewportCoordinateSwizzle0, 0x6402)],
                    after: [(setViewportCoordinateSwizzle0, 0x6420)], draw)
                paths.append(drawn.path)
                emitted += drawn.emitted
            } else {
                paths.append(try await gpu.run(draw).path)
            }
            pictures.append(try await gpu.read(target, bytesPerPixel: 4))
        }
        let transforms: [(String, (Int, Int) -> (Int, Int))] = [
            ("identity", { ($0, $1) }), ("transpose", { ($1, $0) }),
            ("anti_transpose", { (size - 1 - $1, size - 1 - $0) }), ("flip_x", { (size - 1 - $0, $1) }),
            ("flip_y", { ($0, size - 1 - $1) }),
            ("rotate_180", { (size - 1 - $0, size - 1 - $1) }), ("rotate_90", { ($1, size - 1 - $0) }),
            ("rotate_270", { (size - 1 - $1, $0) }),
        ]
        let matched =
            transforms.first { _, map in
                (0..<size).allSatisfy { y in
                    (0..<size).allSatisfy { x in
                        let (sx, sy) = map(x, y)
                        return pictures[1].pixel(x, y, width: size) == pictures[0].pixel(sx, sy, width: size)
                    }
                }
            }?.0 ?? "none"
        var metrics = Metrics()
        metrics.set("matched", matched)
        let exchanged = matched == "transpose" || matched == "anti_transpose"
        return ProbeFinding.counted(["swizzle_wrong": exchanged ? 0 : 1], paths: paths, metrics: metrics)
            .emitting(emitted)
    }
}
