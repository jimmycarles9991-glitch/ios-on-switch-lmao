import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Viewports and scissors of a 64x64 target tiled 4 by 4: viewport i and scissor i cover tile i (the scissor one texel
/// in from each edge of it)
enum ViewportTiles {
    static let size = 64, tile = 16, count = 16

    static func viewports() -> [MTLViewport] {
        (0..<count).map {
            MTLViewport(
                originX: Double($0 % 4 * tile), originY: Double($0 / 4 * tile), width: Double(tile),
                height: Double(tile), znear: 0, zfar: 1)
        }
    }

    static func scissors() -> [MTLScissorRect] {
        (0..<count).map {
            MTLScissorRect(x: $0 % 4 * tile + 1, y: $0 / 4 * tile + 1, width: tile - 2, height: tile - 2)
        }
    }

    /// The tile texel (x, y) lies in, and whether its scissor holds it
    static func tile(_ x: Int, _ y: Int) -> (index: Int, inside: Bool) {
        let i = y / tile * 4 + x / tile
        let (u, v) = (x % tile, y % tile)
        return (i, (1..<(tile - 1)).contains(u) && (1..<(tile - 1)).contains(v))
    }

    static func color(_ i: Int) -> [UInt8] { [UInt8(15 * i), 255 - UInt8(15 * i), UInt8(i % 2 * 255), 255] }

    static func target(_ gpu: GPU) throws -> MTLTexture { try gpu.texture(.rgba8Unorm, size, size) }

    static func pass(_ t: MTLTexture) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = t
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        return pass
    }

    static func wrong(_ gpu: GPU, _ t: MTLTexture, _ want: (Int, Int) -> [UInt8]) async throws -> Int {
        let pixels = try await gpu.read(t, bytesPerPixel: 4)
        var wrong = 0
        for y in 0..<size {
            for x in 0..<size where Array(pixels.pixel(x, y, width: size)) != want(x, y) { wrong += 1 }
        }
        return wrong
    }
}

/// setViewports:count: and setScissorRects:count: of 16 (both identities' drivers keep every one they are given) and a
/// vertex function that writes no [[viewport_array_index]]: every primitive goes through viewport 0 and scissor 0, so a
/// full-target quad fills tile 0 inside its scissor and nothing else.
struct NativeViewportsTest: NativeObjectTest {
    let id = "metal.native.viewports"
    let name = "Viewport and scissor arrays: a vertex function choosing none draws through viewport 0"

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let t = try ViewportTiles.target(gpu)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        var work = NativeWork(objects: [t])
        let drawn: [UInt8] = [255, 255, 0, 255]
        let timing = try await gpu.run { commands in
            let e = commands.makeRenderCommandEncoder(descriptor: ViewportTiles.pass(t))!
            e.setRenderPipelineState(pipeline)
            e.setViewports(ViewportTiles.viewports())
            e.setScissorRects(ViewportTiles.scissors())
            GPU.draw(e, corners: GPU.fullScreen, color: Layering.rgba(drawn))
            e.endEncoding()
        }
        work.paths.append(timing.path)
        let wrong = try await ViewportTiles.wrong(gpu, t) { x, y in
            let (i, inside) = ViewportTiles.tile(x, y)
            return i == 0 && inside ? drawn : [0, 0, 0, 255]
        }
        metrics.set("wrong_texels", wrong)
        if wrong != 0 { work.failures.append("\(wrong) texels outside viewport 0's scissor, or missing inside it") }
        return work
    }
}

/// A vertex function's [[viewport_array_index]]: instance i of a full-target quad through viewport i, each in its own
/// colour, fills tile i inside scissor i; an index at or past the count Metal draws through viewport 0.
struct NativeViewportIndexTest: NativeObjectTest {
    let id = "metal.native.viewport_index"
    let name = "A vertex function chooses among 16 viewports and scissors, each tile its instance's"
    var unavailable: String? { Layering.a13 ? nil : "the A10X's Metal has no [[viewport_array_index]] (Apple5)" }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let t = try ViewportTiles.target(gpu)
        let pipeline = try gpu.render(vertex: "viewport_vertex", fragment: "flat_color_fragment", format: .rgba8Unorm)
        var work = NativeWork(objects: [t])
        let timing = try await gpu.run { commands in
            let e = commands.makeRenderCommandEncoder(descriptor: ViewportTiles.pass(t))!
            e.setRenderPipelineState(pipeline)
            e.setViewports(ViewportTiles.viewports())
            e.setScissorRects(ViewportTiles.scissors())
            var corners = GPU.fullScreen
            var colors = (0..<ViewportTiles.count).map { Layering.rgba(ViewportTiles.color($0)) }
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setVertexBytes(&colors, length: MemoryLayout<SIMD4<Float>>.stride * colors.count, index: 1)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: ViewportTiles.count)
            e.endEncoding()
        }
        work.paths.append(timing.path)
        let wrong = try await ViewportTiles.wrong(gpu, t) { x, y in
            let (i, inside) = ViewportTiles.tile(x, y)
            return inside ? ViewportTiles.color(i) : [0, 0, 0, 255]
        }
        metrics.set("wrong_texels", wrong)
        if wrong != 0 { work.failures.append("\(wrong) texels miss their viewport's instance") }
        return work
    }
}
