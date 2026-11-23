import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Layered passes of more layers than some attachments have (c38-audit m2v-pass-forms/probe/bitsprobe.mm layersMixed,
/// layerprobe.mm), 16x16: a layer-choosing state (r5_layer_vertex) draws each layer in its own colour.
enum LayersPastCases {
    static let side = 16
    static let clear: [UInt8] = [0, 0, 0, 0]

    static func array(_ gpu: GPU, _ format: MTLPixelFormat, layers: Int) throws -> MTLTexture {
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

    /// r5_layer_vertex's colour of layer `l` as RGBA8Unorm
    static func colour(_ l: Int) -> [UInt8] { [UInt8((l + 1) * 51), 64, 191, 255] }

    /// Every layer of `texture`, copied out by one command buffer of blits
    static func slices(_ gpu: GPU, _ texture: MTLTexture, bytesPerPixel: Int) async throws -> [[UInt8]] {
        let image = side * side * bytesPerPixel
        let out = try gpu.buffer(image * texture.arrayLength)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            for slice in 0 ..< texture.arrayLength {
                blit.copy(
                    from: texture, sourceSlice: slice, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                    sourceSize: MTLSize(width: side, height: side, depth: 1), to: out,
                    destinationOffset: slice * image, destinationBytesPerRow: side * bytesPerPixel,
                    destinationBytesPerImage: image
                )
            }
            blit.endEncoding()
        }
        let bytes = UnsafeRawBufferPointer(start: out.contents(), count: image * texture.arrayLength)
        return (0 ..< texture.arrayLength).map { Array(bytes[$0 * image ..< ($0 + 1) * image]) }
    }

    static func texel(_ bytes: [UInt8], _ t: Int, size: Int = 4) -> [UInt8] { Array(bytes[t * size ..< (t + 1) * size])
    }

    /// The quad over columns [x0, x1) of every row at depth z
    static func columns(_ x0: Int, _ x1: Int, z: Float) -> [SIMD4<Float>] {
        GPU.pixelQuad(x0, 0, x1, side, width: side, height: side, z: z)
    }

    /// Each half's texels of a colour layer: k all the clear, d all layer `l`'s colour, x anything else
    static func halves(_ bytes: [UInt8], layer l: Int) -> String {
        halves(bytes, size: 4) { $0 == clear ? "k" : $0 == colour(l) ? "d" : "x" }
    }

    /// Each half of a Depth32Float layer: k all `kept`, w all `written`, x anything else
    static func halves(_ bytes: [UInt8], kept: Float, written: Float) -> String {
        halves(bytes, size: 4) {
            let v = Float(bitPattern: UInt32($0[0]) | UInt32($0[1]) << 8 | UInt32($0[2]) << 16 | UInt32($0[3]) << 24)
            return v == kept ? "k" : v == written ? "w" : "x"
        }
    }

    static func halves(_ bytes: [UInt8], size: Int, _ code: ([UInt8]) -> Character) -> String {
        [0 ..< side / 2, side / 2 ..< side].map { xs -> String in
            let codes = Set((0 ..< side).flatMap { y in xs.map { code(texel(bytes, y * side + $0, size: size)) } })
            return codes.count == 1 ? String(codes.first!) : "x"
        }.joined()
    }

    struct Mixed {
        var c0: [[UInt8]] = []
        var c1: [[UInt8]] = []
        var depth: [[UInt8]] = []
    }

    /// Colour 0 of 4 layers, colour 1 of 2, Depth32Float of 2 cleared to 0.25, a pass of 4 layers: the left half drawn at
    /// z 0.5 under Less (fails everywhere), the right half at z 0.125 with depth writes (passes everywhere)
    static func mixed(_ gpu: GPU) async throws -> Mixed {
        let c0 = try array(gpu, .rgba8Unorm, layers: 4), c1 = try array(gpu, .rgba8Unorm, layers: 2)
        let depth = try array(gpu, .depth32Float, layers: 2)
        let state = try gpu.render(
            vertex: "r5_layer_vertex", fragment: "r5_layer_colours", format: .rgba8Unorm, depth: .depth32Float
        ) {
            $0.colorAttachments[1].pixelFormat = .rgba8Unorm
            $0.inputPrimitiveTopology = .triangle
        }
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .less
        d.isDepthWriteEnabled = true
        guard let less = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            for (j, target) in [c0, c1].enumerated() {
                pass.colorAttachments[j].texture = target
                pass.colorAttachments[j].loadAction = .clear
                pass.colorAttachments[j].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
                pass.colorAttachments[j].storeAction = .store
            }
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 0.25
            pass.depthAttachment.storeAction = .store
            pass.renderTargetArrayLength = 4
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(state)
            e.setDepthStencilState(less)
            var first = UInt32(0)
            e.setVertexBytes(&first, length: 4, index: 1)
            for corners in [columns(0, side / 2, z: 0.5), columns(side / 2, side, z: 0.125)] {
                var corners = corners
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: 4)
            }
            e.endEncoding()
        }
        return Mixed(
            c0: try await slices(gpu, c0, bytesPerPixel: 4), c1: try await slices(gpu, c1, bytesPerPixel: 4),
            depth: try await slices(gpu, depth, bytesPerPixel: 4)
        )
    }
}
