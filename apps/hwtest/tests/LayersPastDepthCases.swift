import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The scenes of gpu.gm20b.layers_past_depth (c38-audit m2v-pass-forms/probe/layerprobe.mm, layers.txt): an RGBA8Unorm
/// array of 4 layers cleared to 0, a pass of 4 layers, a layer-choosing state drawing a full quad in every layer, beside
/// a depth/stencil array of 2 layers a pass of 2 filled first. Each run answers, per layer, whether the draw passed.
enum LayersPastDepthCases {
    struct Format {
        let key: String
        let format: MTLPixelFormat
        let depth: Bool, stencil: Bool

        static let d32f = Format(key: "d32f", format: .depth32Float, depth: true, stencil: false)
        static let d16 = Format(key: "d16", format: .depth16Unorm, depth: true, stencil: false)
        static let d32fs8 = Format(key: "d32fs8", format: .depth32Float_stencil8, depth: true, stencil: true)
        static let s8 = Format(key: "s8", format: .stencil8, depth: false, stencil: true)
    }

    static let compares: [(key: String, function: MTLCompareFunction)] = [
        ("less", .less), ("less_equal", .lessEqual), ("equal", .equal), ("greater", .greater),
        ("greater_equal", .greaterEqual), ("not_equal", .notEqual),
    ]
    static let references: [UInt32] = [0, 1, 5, 9, 128, 255]

    struct Run {
        var depthCompare: MTLCompareFunction = .always
        var stencilCompare: MTLCompareFunction = .always
        var reference: UInt32 = 0
        var z: Float = 1
        var load: MTLLoadAction = .load
        var filledDepth = 0.25
        var filledStencil: UInt32 = 5
        var clearDepth = 0.75
        var clearStencil: UInt32 = 9
    }

    static func state(_ gpu: GPU, _ f: Format) throws -> MTLRenderPipelineState {
        try gpu.render(
            vertex: "r5_layer_vertex", fragment: "r5_layer_colour", format: .rgba8Unorm,
            depth: f.depth ? f.format : .invalid, stencil: f.stencil ? f.format : .invalid
        ) { $0.inputPrimitiveTopology = .triangle }
    }

    /// Per layer: 1 every texel drawn, 0 none, m some
    static func drawn(_ gpu: GPU, _ f: Format, _ state: MTLRenderPipelineState, _ run: Run) async throws -> String {
        let colour = try LayersPastCases.array(gpu, .rgba8Unorm, layers: 4)
        let zeta = try LayersPastCases.array(gpu, f.format, layers: 2)
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = run.depthCompare
        let face = MTLStencilDescriptor()
        face.stencilCompareFunction = run.stencilCompare
        d.frontFaceStencil = face
        d.backFaceStencil = face
        guard let tests = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no state") }
        try await gpu.run { commands in
            let fill = MTLRenderPassDescriptor()
            attach(fill, f, zeta, load: .clear, depth: run.filledDepth, stencil: run.filledStencil)
            fill.renderTargetArrayLength = 2
            commands.makeRenderCommandEncoder(descriptor: fill)!.endEncoding()
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].storeAction = .store
            attach(pass, f, zeta, load: run.load, depth: run.clearDepth, stencil: run.clearStencil)
            pass.renderTargetArrayLength = 4
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(state)
            e.setDepthStencilState(tests)
            e.setStencilReferenceValue(run.reference)
            var corners = GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: run.z)
            var first = UInt32(0)
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setVertexBytes(&first, length: 4, index: 1)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: 4)
            e.endEncoding()
        }
        let layers = try await LayersPastCases.slices(gpu, colour, bytesPerPixel: 4)
        return String(
            layers.enumerated().map { l, bytes -> Character in
                let texels = (0 ..< bytes.count / 4).map { LayersPastCases.texel(bytes, $0) }
                if texels.allSatisfy({ $0 == LayersPastCases.colour(l) }) { return "1" }
                return texels.allSatisfy { $0 == LayersPastCases.clear } ? "0" : "m"
            }
        )
    }

    static func attach(
        _ pass: MTLRenderPassDescriptor, _ f: Format, _ zeta: MTLTexture, load: MTLLoadAction, depth: Double,
        stencil: UInt32
    ) {
        if f.depth {
            pass.depthAttachment.texture = zeta
            pass.depthAttachment.loadAction = load
            pass.depthAttachment.clearDepth = depth
            pass.depthAttachment.storeAction = .store
        }
        if f.stencil {
            pass.stencilAttachment.texture = zeta
            pass.stencilAttachment.loadAction = load
            pass.stencilAttachment.clearStencil = stencil
            pass.stencilAttachment.storeAction = .store
        }
    }

    /// One pass into a 1280x720 RGBA8Unorm beside a Depth32Float filled with 0.75 in a command buffer before it, the
    /// depth loaded as `load` with clear depth 0.25: its GPU time, the least of `runs`
    static func passMilliseconds(_ gpu: GPU, load: MTLLoadAction, runs: Int = 5) async throws -> Double {
        let colour = try gpu.texture(.rgba8Unorm, 1280, 720, usage: [.renderTarget])
        let depth = try gpu.texture(.depth32Float, 1280, 720, usage: [.renderTarget])
        let state = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm, depth: .depth32Float)
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .less
        guard let less = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
        var least = Double.infinity
        for _ in 0 ..< runs {
            try await gpu.run { commands in
                let fill = MTLRenderPassDescriptor()
                attach(fill, .d32f, depth, load: .clear, depth: 0.75, stencil: 0)
                commands.makeRenderCommandEncoder(descriptor: fill)!.endEncoding()
            }
            let timing = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = colour
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                attach(pass, .d32f, depth, load: load, depth: 0.25, stencil: 0)
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(state)
                e.setDepthStencilState(less)
                GPU.draw(e, corners: GPU.fullScreen, color: SIMD4(1, 1, 1, 1))
                e.endEncoding()
            }
            least = min(least, timing.gpuMilliseconds)
        }
        return least
    }
}
