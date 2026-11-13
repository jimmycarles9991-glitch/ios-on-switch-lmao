import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Zcull (drivers/TegraGM20BMetal/raw/runtime/raw_zcull.h, drivers/TegraGM20B/TegraGM20BZcull.h): passes that clear a
/// one-sample depth target get a zcull region. A near quad over the left half, then far quads over everything: the left
/// half keeps the near colour, the right the far one, with LESS and with reversed depth (GREATER from a 0 clear); a
/// GREATER draw after LESS ones invalidates the region and still draws where it passes. The facade counts the regions
/// and the invalidation; the same far quads with the region invalidated before every draw give the GPU time zcull saves.
struct ZcullTest: HWTest {
    let id = "gpu.raw.zcull"
    let name = "Zcull rejects hidden quads; a direction flip invalidates it; pixels exact"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let width = 1280
    static let height = 720
    static let farQuads = 64
    static let rounds = 4
    static let red = SIMD4<Float>(1, 0, 0, 1), green = SIMD4<Float>(0, 1, 0, 1), blue = SIMD4<Float>(0, 0, 1, 1)

    struct Draw {
        let corners: [SIMD4<Float>]
        let colour: SIMD4<Float>
        let state: MTLDepthStencilState
        let count: Int
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let target = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.renderTarget, .shaderRead])
        let depth = try gpu.texture(
            .depth32Float, Self.width, Self.height, usage: [.renderTarget], storage: .memoryless)
        let pipeline = try gpu.render(fragment: "exact_fragment", format: .bgra8Unorm, depth: .depth32Float)
        func state(_ compare: MTLCompareFunction) -> MTLDepthStencilState {
            let d = MTLDepthStencilDescriptor()
            d.depthCompareFunction = compare
            d.isDepthWriteEnabled = true
            return gpu.device.makeDepthStencilState(descriptor: d)!
        }
        let (w, h) = (Self.width, Self.height)
        let left = { (z: Float) in GPU.pixelQuad(0, 0, w / 2, h, width: w, height: h, z: z) }
        let all = { (z: Float) in GPU.pixelQuad(0, 0, w, h, width: w, height: h, z: z) }
        func encode(_ clearDepth: Double, _ draws: [Draw]) -> (MTLCommandBuffer) -> Void {
            { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = clearDepth
                pass.depthAttachment.storeAction = .dontCare
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                for draw in draws {
                    var corners = draw.corners
                    var colour = draw.colour
                    encoder.setDepthStencilState(draw.state)
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    encoder.setFragmentBytes(&colour, length: 16, index: 0)
                    for _ in 0..<draw.count {
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    }
                }
                encoder.endEncoding()
            }
        }
        var metrics = Metrics()
        var wrong: [String] = []
        var paths: Set<String> = []
        let before = RawFacade.counters(of: gpu.device)
        let less = encode(
            1,
            [
                Draw(corners: left(0.25), colour: Self.red, state: state(.less), count: 1),
                Draw(corners: all(0.75), colour: Self.green, state: state(.less), count: Self.farQuads),
            ])
        let reversed = encode(
            0,
            [
                Draw(corners: left(0.75), colour: Self.red, state: state(.greater), count: 1),
                Draw(corners: all(0.25), colour: Self.green, state: state(.greaterEqual), count: Self.farQuads),
            ])
        let flipped = encode(
            1,
            [
                Draw(corners: left(0.25), colour: Self.red, state: state(.less), count: 1),
                Draw(corners: all(0.75), colour: Self.green, state: state(.lessEqual), count: 1),
                Draw(corners: all(0.5), colour: Self.blue, state: state(.greater), count: 1),
            ])
        for (label, pass, leftColour) in [
            ("less", less, Self.red), ("reversed", reversed, Self.red),
            ("flipped", flipped, Self.blue),
        ] {
            paths.insert(try await gpu.run(pass).path)
            let raster = try await gpu.raster(target, .bgra8Unorm)
            var bad = 0
            for y in stride(from: 0, to: h, by: 3) {
                for x in stride(from: 0, to: w, by: 3) {
                    let want = x < w / 2 ? leftColour : Self.green
                    for c in 0..<4 where abs(raster[x, y, c] - want[c]) > 0.002 { bad += 1 }
                }
            }
            metrics.set("\(label)_wrong", bad)
            if bad != 0 { wrong.append(label) }
        }
        let after = RawFacade.counters(of: gpu.device)
        let passes = (after["zcull_passes"] ?? 0) - (before["zcull_passes"] ?? 0)
        let invalidations = (after["zcull_invalidations"] ?? 0) - (before["zcull_invalidations"] ?? 0)
        metrics.set("zcull_passes", passes)
        metrics.set("zcull_invalidations", invalidations)
        if passes < 3 { wrong.append("zcull_passes") }
        if invalidations < 1 { wrong.append("zcull_invalidations") }
        var onMs = 0.0, offMs = 0.0, emitted = 0
        for _ in 0..<Self.rounds {
            let on = try await gpu.run(less)
            onMs += on.gpuMilliseconds
            paths.insert(on.path)
            let off = try await RawMethods.draw(gpu, before: [(0x1958, 0)], after: [], less)
            offMs += off.gpuMilliseconds
            emitted += off.emitted
            paths.insert(off.path)
        }
        metrics.set("zcull_gpu_ms", onMs, digits: 3)
        metrics.set("invalidated_gpu_ms", offMs, digits: 3)
        metrics.set("invalidated_emitted", emitted)
        metrics.set("paths", paths.sorted().joined(separator: ","))
        if paths != ["raw"] { wrong.append("paths") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
