import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// c38-audit m2v-pass-forms spec 4: whether SET_COLOR_TARGET_THIRD_DIMENSION alone keeps a layered draw out of the layers
/// past a target's own, which would let the driver bind a shorter attachment in place of today's backing of the pass's
/// layers (pass_begin.cpp shortOf; gpu.gm20b.layers_past checks that). Two 16x16 RGBA8Unorm arrays of 4 layers, both
/// loaded: target 0 zeros, target 1 zeros in layers 0-1 and 0x5a bytes in layers 2-3, bound with THIRD_DIMENSION 2 for
/// each draw (0x0858, set back to 4 after it). Four one-instance draws in layer order, draw l into layer l in its own
/// colour (r5_layer_vertex). The claim, Apple's behaviour (gpu.gm20b.rtal_past_layers): target 1 keeps 0x5a in layers 2-3
/// and draw 0's and draw 1's colours in layers 0-1. past_count_layer2/3: 0 kept, 1 the layer's own colour, 2 layer 1's,
/// 3 layer 0's, -1 another; past_count_c0 and _c1 each layer's holder, layers 0 to 3 (the draw whose colour it holds, k
/// the bytes it was filled with, x another), which tells a draw dropped past the count from one clamped onto layer 1.
enum X1LayerPastCountProbe {
    static let setColorTargetThirdDimension1: UInt32 = 0x0858
    static let side = 16

    static func colour(_ l: Int) -> [UInt8] { [UInt8((l + 1) * 51), 64, 191, 255] }

    static func array(_ gpu: GPU) throws -> MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: side, height: side, mipmapped: false
        )
        d.textureType = .type2DArray
        d.arrayLength = 4
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .private
        guard let t = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no RGBA8Unorm array") }
        return t
    }

    static func holder(_ bytes: [UInt8], filled: UInt8) -> String {
        let texels = Set(stride(from: 0, to: bytes.count, by: 4).map { Array(bytes[$0 ..< $0 + 4]) })
        guard texels.count == 1, let texel = texels.first else { return "x" }
        if texel == [UInt8](repeating: filled, count: 4) { return "k" }
        return (0 ..< 4).first { colour($0) == texel }.map(String.init) ?? "x"
    }

    /// Target 0 zeros, target 1 zeros in layers 0-1 and 0x5a bytes in layers 2-3
    static func targets(_ gpu: GPU) async throws -> [MTLTexture] {
        let targets = [try array(gpu), try array(gpu)]
        let image = side * side * 4
        for slice in 0 ..< 4 {
            try await gpu.upload(
                [UInt8](repeating: 0, count: image), to: targets[0], bytesPerRow: side * 4, slice: slice
            )
            try await gpu.upload(
                [UInt8](repeating: slice < 2 ? 0 : 0x5a, count: image), to: targets[1], bytesPerRow: side * 4,
                slice: slice
            )
        }
        return targets
    }

    static func state(_ gpu: GPU) throws -> MTLRenderPipelineState {
        try gpu.render(vertex: "r5_layer_vertex", fragment: "r5_layer_colours", format: .rgba8Unorm) {
            $0.colorAttachments[1].pixelFormat = .rgba8Unorm
            $0.inputPrimitiveTopology = .triangle
        }
    }

    static func encode(_ targets: [MTLTexture], _ state: MTLRenderPipelineState) -> (MTLCommandBuffer) -> Void {
        { commands in
            let pass = MTLRenderPassDescriptor()
            for (j, target) in targets.enumerated() {
                pass.colorAttachments[j].texture = target
                pass.colorAttachments[j].loadAction = .load
                pass.colorAttachments[j].storeAction = .store
            }
            pass.renderTargetArrayLength = 4
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            for l in 0 ..< 4 {
                var first = UInt32(l)
                e.setVertexBytes(&first, length: 4, index: 1)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            e.endEncoding()
        }
    }

    static func holders(_ gpu: GPU, _ targets: [MTLTexture]) async throws -> [[String]] {
        var holders: [[String]] = [[], []]
        for (j, target) in targets.enumerated() {
            for slice in 0 ..< 4 {
                let filled: UInt8 = j == 1 && slice >= 2 ? 0x5a : 0
                holders[j].append(holder(try await gpu.read(target, bytesPerPixel: 4, slice: slice), filled: filled))
            }
        }
        return holders
    }

    static func finding(_ holders: [[String]], path: String) -> ProbeFinding {
        var metrics = Metrics()
        for l in [2, 3] {
            let code = ["k": 0, String(l): 1, "1": 2, "0": 3][holders[1][l]] ?? -1
            metrics.set("past_count_layer\(l)", code)
        }
        metrics.set("past_count_c0", holders[0].joined(separator: "/"))
        metrics.set("past_count_c1", holders[1].joined(separator: "/"))
        metrics.set("path", path)
        let holds = holders[0] == ["0", "1", "2", "3"] && holders[1] == ["0", "1", "k", "k"]
        if path != "gm20b" { return ProbeFinding(.inconclusive, metrics, note: "not on the driver: \(path)") }
        return ProbeFinding(holds ? .pass : .fail, metrics, note: holds ? "" : "target 1 \(holders[1])")
    }

    static let probe = X1Probe(
        id: "gpu.x1.layer_past_count", name: "THIRD_DIMENSION alone keeps a layered draw out of the layers past it",
        probe: "layer_past_count", risky: true
    ) { gpu in
        let targets = try await targets(gpu)
        let drawn = try await GM20BMethods.draw(
            gpu, before: [(setColorTargetThirdDimension1, 2)], after: [(setColorTargetThirdDimension1, 4)],
            encode(targets, try state(gpu))
        )
        return finding(try await holders(gpu, targets), path: drawn.path).emitting(drawn.emitted)
    }
}
