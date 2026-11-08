import Foundation
import HWTestCore
import Metal

/// Instanced draws, indexed and not, with base vertex and base instance: 16- and 32-bit indices, an index buffer
/// offset, a base vertex past 65535 under 16-bit indices (it is added after the index is read, never truncated to
/// it). Every instance is a pixel-aligned rectangle of its own colour, so the CPU reference is exact. The app's
/// pipelines are no framework's, so on the raw facade these draw on m2v (paths= says so); the raw path's own instanced
/// draws are Core Animation's and RenderBox's, which the UI scenes reach.
struct InstancedDrawScene: GPUScene {
    let id = "gpu.draw.instanced"
    let name = "Instanced and indexed draws with base vertex and base instance"

    static let shapes: [(base: Int, width: Float, height: Float)] = [
        (0, 6, 4), (4, 4, 7), (8, 9, 3), (1000, 5, 5), (70000, 3, 8),
    ]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let (width, height) = (256, 128)
        var corners = [SIMD2<Float>](repeating: SIMD2(-100, -100), count: 70004)
        for shape in Self.shapes {
            let (w, h) = (shape.width, shape.height)
            let quad: [SIMD2<Float>] = [SIMD2(0, 0), SIMD2(w, 0), SIMD2(0, h), SIMD2(w, h)]
            corners.replaceSubrange(shape.base..<(shape.base + 4), with: quad)
        }
        let offsets = (0..<64).map { (i: Int) -> SIMD2<Float> in
            let (x, y): (Int, Int) = (4 + (i % 16) * 15, 4 + (i / 16) * 15)
            return SIMD2<Float>(Float(x), Float(y))
        }
        let colors = (0..<64).map { (i: Int) -> SIMD4<Float> in
            let (r, g, b): (Int, Int, Int) = ((i * 37) % 256, (i * 91 + 40) % 256, 255 - i * 3)
            return SIMD4<Float>(Float(r) / 255, Float(g) / 255, Float(b) / 255, 1)
        }
        let pipeline = try gpu.render(vertex: "instanced_vertex", fragment: "color_fragment", format: .rgba8Unorm)
        let cornerBuffer = try gpu.buffer(corners)
        let offsetBuffer = try gpu.buffer(offsets)
        let colorBuffer = try gpu.buffer(colors)
        let indices16 = try gpu.buffer([UInt16](arrayLiteral: 0, 1, 2, 2, 1, 3))
        let indices32 = try gpu.buffer([UInt32](arrayLiteral: 99, 99, 99, 0, 1, 2, 2, 1, 3))
        let target = try gpu.texture(.rgba8Unorm, width, height)
        var drawn: [(Int, Int, Int)] = []
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setVertexBuffer(cornerBuffer, offset: 0, index: 0)
            encoder.setVertexBuffer(offsetBuffer, offset: 0, index: 1)
            encoder.setVertexBuffer(colorBuffer, offset: 0, index: 2)
            var size = SIMD2<Float>(Float(width), Float(height))
            encoder.setVertexBytes(&size, length: MemoryLayout<SIMD2<Float>>.stride, index: 3)
            let indexed: [(MTLIndexType, MTLBuffer, Int, Int, Int, Int)] = [
                (.uint16, indices16, 0, 0, 0, 2), (.uint16, indices16, 0, 1, 2, 3), (.uint32, indices32, 12, 2, 10, 2),
                (.uint16, indices16, 0, 3, 30, 1), (.uint16, indices16, 0, 4, 40, 2),
            ]
            for (type, buffer, offset, shape, first, count) in indexed {
                encoder.drawIndexedPrimitives(
                    type: .triangle, indexCount: 6, indexType: type, indexBuffer: buffer, indexBufferOffset: offset,
                    instanceCount: count, baseVertex: Self.shapes[shape].base, baseInstance: first)
                drawn.append((shape, first, count))
            }
            encoder.drawPrimitives(
                type: .triangleStrip, vertexStart: 8, vertexCount: 4, instanceCount: 4, baseInstance: 20)
            drawn.append((2, 20, 4))
            encoder.endEncoding()
        }
        var want = Raster(width: width, height: height, channels: 4) { _, _, c in c == 3 ? 1 : 0 }
        for (shape, first, count) in drawn {
            for instance in first..<(first + count) {
                let (x0, y0) = (Int(offsets[instance].x), Int(offsets[instance].y))
                for y in y0..<(y0 + Int(Self.shapes[shape].height)) {
                    for x in x0..<(x0 + Int(Self.shapes[shape].width)) {
                        for c in 0..<4 { want[x, y, c] = colors[instance][c] }
                    }
                }
            }
        }
        result.compare("instanced draws", try await gpu.raster(target, .rgba8Unorm), want, tolerance: 1.01 / 255)
        return result
    }
}

/// Render to a private texture (block-linear) with the app's pipeline, then sample it: nearest and mirrored, bilinear
/// halfway between texel centres (an exact average), and copied whole by the blit engine (the raw path's own copy on
/// the facade, reading what m2v's draw left through the alias of the same memory).
struct RenderThenSampleScene: GPUScene {
    let id = "gpu.draw.render_then_sample"
    let name = "Render to texture, then sample and copy it"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let size = 128
        let source = try gpu.texture(.rgba8Unorm, size, size)
        let solid = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        let rects: [(UploadRect, [Float])] = [
            (UploadRect(x: 0, y: 0, width: 64, height: 64), [1, 0, 0, 1]),
            (UploadRect(x: 64, y: 0, width: 64, height: 64), [0, 1, 0, 1]),
            (UploadRect(x: 0, y: 64, width: 64, height: 64), [0, 0, 1, 1]),
            (UploadRect(x: 64, y: 64, width: 64, height: 64), [1, 1, 1, 1]),
            (UploadRect(x: 37, y: 5, width: 2, height: 2), [0.2, 0.4, 0.6, 1]),
            (UploadRect(x: 99, y: 101, width: 3, height: 1), [0.8, 0.6, 0.2, 1]),
            (UploadRect(x: 10, y: 70, width: 1, height: 40), [0, 0, 0, 1]),
        ]
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = source
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(solid)
            for (r, color) in rects {
                GPU.draw(
                    encoder, corners: GPU.pixelQuad(r.x, r.y, r.x + r.width, r.y + r.height, width: size, height: size),
                    color: SIMD4(color[0], color[1], color[2], color[3]))
            }
            encoder.endEncoding()
        }
        var model = Raster(width: size, height: size, channels: 4) { _, _, c in c == 3 ? 1 : 0 }
        for (r, color) in rects {
            for y in r.y..<(r.y + r.height) {
                for x in r.x..<(r.x + r.width) { for c in 0..<4 { model[x, y, c] = color[c] } }
            }
        }
        result.compare("rendered", try await gpu.raster(source, .rgba8Unorm), model, tolerance: 1.01 / 255)

        let half = Float(0.5) / Float(size)
        let samples: [(String, String, [SIMD2<Float>], Raster)] = [
            (
                "nearest mirrored", "nearest_fragment", [SIMD2(1, 1), SIMD2(0, 1), SIMD2(1, 0), SIMD2(0, 0)],
                Raster(width: size, height: size, channels: 4) { x, y, c in model[size - 1 - x, y, c] }
            ),
            (
                "bilinear midpoints", "linear_fragment",
                [SIMD2(half, 1), SIMD2(1 + half, 1), SIMD2(half, 0), SIMD2(1 + half, 0)],
                Raster(width: size, height: size, channels: 4) { x, y, c in
                    (model[x, y, c] + model[min(x + 1, size - 1), y, c]) / 2
                }
            ),
        ]
        for (label, fragment, uvs, want) in samples {
            let target = try gpu.texture(.rgba8Unorm, size, size)
            let pipeline = try gpu.render(vertex: "textured_vertex", fragment: fragment, format: .rgba8Unorm)
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .dontCare
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                var corners = GPU.fullScreen
                var uvs = uvs
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setVertexBytes(&uvs, length: MemoryLayout<SIMD2<Float>>.stride * 4, index: 1)
                encoder.setFragmentTexture(source, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            result.compare(label, try await gpu.raster(target, .rgba8Unorm), want, tolerance: 1.51 / 255)
        }

        let copy = try gpu.texture(.rgba8Unorm, size, size, usage: [.shaderRead])
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(from: source, to: copy)
            blit.endEncoding()
        }
        result.compare("blit copy", try await gpu.raster(copy, .rgba8Unorm), model, tolerance: 1.01 / 255)
        return result
    }
}
