import HWTestCore
import HWTestGPU
import Metal

/// gpu.msaa.resolve_cost's split of a 4x resolve's 11.5-12 ms (four times nakstat's estimate of resolve_bgra8_frag's
/// instructions) between its hypotheses, each a full-screen draw into a 720x1280 BGRA8 target less a bare pass:
/// ms4 (four multisampled fetches a pixel, averaged in floats), ms1 (one: what the three more fetches cost), grid4_tiled
/// (the same 14.7 MB as a one-sample 1440x2560 block-linear texture, four fetches: the multisampled fetch against a plain
/// one over the same bytes and tiling) and grid4_pitch (that texture over a buffer, pitch-linear: the sample grid's layout).
/// The resolve arms against ms4 are the program's own instructions; memoryless against private, the source's compression.
enum ResolveArms {
    static let width = ResolveCostTest.width
    static let height = ResolveCostTest.height

    static func measure(_ gpu: GPU, into metrics: inout Metrics) async throws {
        let target = try gpu.texture(.bgra8Unorm, width, height)
        let ms = try gpu.texture(.bgra8Unorm, width, height, usage: [.renderTarget, .shaderRead], samples: 4)
        let tiled = try gpu.texture(.bgra8Unorm, width * 2, height * 2)
        let rowBytes = width * 2 * 4
        let linear = try gpu.buffer(rowBytes * height * 2)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width * 2, height: height * 2, mipmapped: false)
        descriptor.usage = [.shaderRead, .renderTarget]
        descriptor.storageMode = .shared
        guard let pitch = linear.makeTexture(descriptor: descriptor, offset: 0, bytesPerRow: rowBytes) else {
            metrics.set("arms", "no pitch-linear texture over a buffer")
            return
        }
        for source in [ms, tiled, pitch] { try await fill(gpu, source) }
        let bare = try await median(gpu, target, nil, nil)
        metrics.set("arm_bare_ms", bare, digits: 3)
        let arms: [(String, String, MTLTexture)] = [
            ("ms4", "arms_ms4_frag", ms), ("ms1", "arms_ms1_frag", ms), ("grid4_tiled", "arms_grid4_frag", tiled),
            ("grid4_pitch", "arms_grid4_frag", pitch),
        ]
        for (label, fragment, source) in arms {
            let pipeline = try gpu.render(vertex: "arms_vertex", fragment: fragment, format: .bgra8Unorm)
            let ms = try await median(gpu, target, pipeline, source)
            metrics.set("arm_\(label)_ms", max(0, ms - bare), digits: 3)
        }
    }

    static func fill(_ gpu: GPU, _ texture: MTLTexture) async throws {
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = ResolveCostTest.clear
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
        }
    }

    static func median(
        _ gpu: GPU, _ target: MTLTexture, _ pipeline: MTLRenderPipelineState?, _ source: MTLTexture?
    )
        async throws -> Double
    {
        var times: [Double] = []
        for _ in 0..<ResolveCostTest.runs {
            let timing = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .dontCare
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                if let pipeline, let source {
                    encoder.setRenderPipelineState(pipeline)
                    encoder.setFragmentTexture(source, index: 0)
                    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                }
                encoder.endEncoding()
            }
            times.append(timing.gpuMilliseconds)
        }
        return times.sorted()[times.count / 2]
    }
}
