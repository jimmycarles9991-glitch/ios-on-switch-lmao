import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import Metal

/// The scenes of gpu.gm20b.depth16_layered_tests: a fetching state with a Depth16Unorm depth (the driver tests depth in
/// its shader, TegraGM20BMetalCubic.mm fragmentTests) drawing an off-centre rectangle at z 0.5 under Less with depth writes.
/// Layered: a 64x64 Depth16Unorm 2D array of 3 layers, layer l cleared to 0.25 + 0.3 l and loaded by a pass of 3 layers
/// into an RGBA8Unorm array cleared black. Turned: one 96x64 layer cleared to 0.55, loaded by a pass into a plane Metal's
/// rotation SPI turns once (RotatedCells), against the same pass into a plain texture.
enum Depth16LayeredCases {
    static let side = 64, layers = 3
    static let rect = (x0: 8, y0: 16, x1: 40, y1: 48)
    static let turnedRect = (x0: 10, y0: 5, x1: 50, y1: 45)

    struct Layered {
        var depth: [[UInt16]] = []
        var colour: [[UInt8]] = []
    }

    struct Turned {
        var depthOff = 0
        var colourOff = 0
        var path = ""
    }

    static func depthState(_ gpu: GPU) throws -> MTLDepthStencilState {
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .less
        d.isDepthWriteEnabled = true
        guard let state = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
        return state
    }

    static func array(_ gpu: GPU, _ format: MTLPixelFormat) throws -> MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: side, height: side, mipmapped: false
        )
        d.textureType = .type2DArray
        d.arrayLength = layers
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .private
        guard let t = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no \(format.rawValue) array") }
        return t
    }

    static func cleared(_ commands: MTLCommandBuffer, _ depth: MTLTexture, slice: Int, to value: Double) {
        let pass = MTLRenderPassDescriptor()
        pass.depthAttachment.texture = depth
        pass.depthAttachment.slice = slice
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = value
        pass.depthAttachment.storeAction = .store
        commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
    }

    static func layered(_ gpu: GPU) async throws -> Layered {
        let depth = try array(gpu, .depth16Unorm), colour = try array(gpu, .rgba8Unorm)
        let state = try gpu.render(
            vertex: "r5_layer_vertex", fragment: "r5_layer_fetch", format: .rgba8Unorm, depth: .depth16Unorm
        ) { $0.inputPrimitiveTopology = .triangle }
        let less = try depthState(gpu)
        try await gpu.run { commands in
            for l in 0 ..< layers { cleared(commands, depth, slice: l, to: 0.25 + 0.3 * Double(l)) }
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .load
            pass.depthAttachment.storeAction = .store
            pass.renderTargetArrayLength = layers
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(state)
            e.setDepthStencilState(less)
            var corners = GPU.pixelQuad(rect.x0, rect.y0, rect.x1, rect.y1, width: side, height: side, z: 0.5)
            var first = UInt32(0)
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setVertexBytes(&first, length: 4, index: 1)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: layers)
            e.endEncoding()
        }
        var result = Layered()
        for l in 0 ..< layers {
            let bytes = try await gpu.read(depth, bytesPerPixel: 2, slice: l)
            result.depth.append((0 ..< side * side).map { bytes.loaded(as: UInt16.self, at: $0) })
            result.colour.append(try await gpu.read(colour, bytesPerPixel: 4, slice: l))
        }
        return result
    }

    static func inside(_ texel: Int) -> Bool {
        let x = texel % side, y = texel / side
        return x >= rect.x0 && x < rect.x1 && y >= rect.y0 && y < rect.y1
    }

    /// Depth texels of layer `l` off Metal's: inside the rectangle the draw's 0.5 within a code of 32768 where it passes
    /// (layers 1 and 2), the layer's clear code everywhere else
    static func depthOff(_ r: Layered, layer l: Int, clear: UInt16) -> Int {
        r.depth[l].indices.filter { t in
            let code = Int(r.depth[l][t])
            return inside(t) && l > 0 ? abs(code - 32768) > 1 : code != Int(clear)
        }.count
    }

    /// Colour texels off the draw's reach: written inside the rectangle of layers 1 and 2, black everywhere else
    static func colourOff(_ r: Layered) -> Int {
        (0 ..< layers).map { l in
            (0 ..< side * side).filter { t in
                let written = r.colour[l][4 * t ..< 4 * t + 3].contains { $0 != 0 }
                return written != (l > 0 && inside(t))
            }.count
        }.reduce(0, +)
    }

    static func turned(_ gpu: GPU) async throws -> Turned? {
        let states = try RotatedStates(gpu)
        let cells = RotatedCells(gpu: gpu, states: states, turn: 1, width: 96, height: 64, aligned: true)
        guard let plane = try cells.makePlane() else { return nil }
        let state = try gpu.render(fragment: "fetch_add_fragment", format: .bgra8Unorm, depth: .depth16Unorm)
        let less = try depthState(gpu)
        let plain = try gpu.texture(.bgra8Unorm, cells.width, cells.height)
        var depths: [[UInt8]] = []
        var result = Turned()
        for target in [plain, plane.texture] {
            let depth = try gpu.texture(.depth16Unorm, cells.width, cells.height)
            let path = try await gpu.run { commands in
                cleared(commands, depth, slice: 0, to: 0.55)
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .load
                pass.depthAttachment.storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(state)
                e.setDepthStencilState(less)
                let r = turnedRect
                GPU.draw(
                    e, corners: GPU.pixelQuad(r.x0, r.y0, r.x1, r.y1, width: cells.width, height: cells.height),
                    color: SIMD4(0.25, 0.5, 0.75, 1)
                )
                e.endEncoding()
            }.path
            if target === plane.texture { result.path = path }
            depths.append(try await gpu.read(depth, bytesPerPixel: 2))
        }
        result.depthOff =
            stride(from: 0, to: depths[0].count, by: 2).filter {
                depths[0][$0] != depths[1][$0] || depths[0][$0 + 1] != depths[1][$0 + 1]
            }.count
        let expected = try await gpu.read(plain, bytesPerPixel: 4)
        guard plane.surface.lock(options: .readOnly, seed: nil) == 0 else { throw TestFailure("IOSurfaceLock failed") }
        defer { plane.surface.unlock(options: .readOnly, seed: nil) }
        for y in 0 ..< cells.height {
            for x in 0 ..< cells.width {
                let (px, py) = cells.stored(x, y)
                let got = UnsafeRawBufferPointer(
                    start: plane.surface.baseAddress + py * plane.rowBytes + px * 4, count: 4
                )
                if !got.elementsEqual(expected.pixel(x, y, width: cells.width)) { result.colourOff += 1 }
            }
        }
        return result
    }
}
