import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// RG8Unorm_sRGB as a colour target, against RGBA8Unorm_sRGB, which the driver renders natively: 64x64 targets of
/// each given the same passes, a draw of a per-pixel sweep (texel i = x + 64y: i / 4095, (4095 - i) / 4095, the
/// fraction of 0.37i, 0.5) over a clear (sweep), clears to (0.5, 0.25), (0.0031, 0.04045), (0.2140, 0.7354), (1/3, 2/3)
/// and (0.99, 0.001) (clears), and the sweep loaded and blended over by (0.8, 0.1, 0.3) at source alpha 0.37 (blend).
/// Every RG8 texel equals the RGBA8 one's first two bytes, tolerance 0 (an M4 Max: m2v-passes3 probe/p3probe.mm
/// rg8_srgb, out3.txt, 0 of 8192 channels; this scene on an M4 Max: 0 off in each). sweep_off, clears_off, blend_off
/// channels off; every command buffer raw, failed_cbs=0.
struct Rg8SrgbTargetTest: HWTest {
    let id = "gpu.gm20b.rg8_srgb_target"
    let name = "RG8Unorm_sRGB targets store what RGBA8Unorm_sRGB stores in its first two bytes, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let side = 64
    static let clears: [(Double, Double)] = [
        (0.5, 0.25), (0.0031, 0.04045), (0.2140, 0.7354), (1 / 3, 2 / 3), (0.99, 0.001),
    ]
    static let sweep: [SIMD4<Float>] = (0 ..< side * side).map { i in
        let f = Float(i)
        return SIMD4(f / 4095, (4095 - f) / 4095, (f * 0.37) - (f * 0.37).rounded(.down), 0.5)
    }
    static let over = SIMD4<Float>(0.8, 0.1, 0.3, 0.37)

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        let parts: [(key: String, run: (GPU, [MTLTexture]) async throws -> Int)] = [
            ("sweep", Self.swept), ("clears", Self.cleared), ("blend", Self.blended),
        ]
        for part in parts {
            do {
                let targets = try [MTLPixelFormat.rg8Unorm_srgb, .rgba8Unorm_srgb].map {
                    try gpu.texture($0, Self.side, Self.side)
                }
                let off = try await part.run(gpu, targets)
                metrics.set(part.key + "_off", off)
                checks.append(("\(part.key): every RG8 texel the RGBA8 one's first two bytes", off == 0))
            } catch {
                checks.append(("\(part.key): \(GPU.describe(error))", false))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func pass(
        _ gpu: GPU, _ target: MTLTexture, load: MTLLoadAction, clear: MTLClearColor, values: [SIMD4<Float>]? = nil,
        blend: SIMD4<Float>? = nil
    ) async throws {
        let state = try gpu.render(
            fragment: values == nil ? "solid_fragment" : "texel_values", format: target.pixelFormat
        ) {
            guard blend != nil, let a = $0.colorAttachments[0] else { return }
            a.isBlendingEnabled = true
            a.sourceRGBBlendFactor = .sourceAlpha
            a.destinationRGBBlendFactor = .oneMinusSourceAlpha
            a.sourceAlphaBlendFactor = .one
            a.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        }
        let data = try gpu.buffer(values ?? [])
        var width = UInt32(side)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = load
            pass.colorAttachments[0].clearColor = clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            if let blend {
                encoder.setRenderPipelineState(state)
                GPU.draw(encoder, corners: GPU.fullScreen, color: blend)
            } else if values != nil {
                encoder.setRenderPipelineState(state)
                var corners = GPU.fullScreen
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentBuffer(data, offset: 0, index: 0)
                encoder.setFragmentBytes(&width, length: 4, index: 1)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
    }

    static func off(_ gpu: GPU, _ targets: [MTLTexture]) async throws -> Int {
        let rg = try await gpu.read(targets[0], bytesPerPixel: 2)
        let rgba = try await gpu.read(targets[1], bytesPerPixel: 4)
        return (0 ..< side * side * 2).filter { rg[$0] != rgba[$0 / 2 * 4 + $0 % 2] }.count
    }

    static func swept(_ gpu: GPU, _ targets: [MTLTexture]) async throws -> Int {
        for t in targets {
            try await pass(
                gpu, t, load: .clear, clear: MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1), values: sweep
            )
        }
        return try await off(gpu, targets)
    }

    static func cleared(_ gpu: GPU, _ targets: [MTLTexture]) async throws -> Int {
        var off = 0
        for (r, g) in clears {
            for t in targets {
                try await pass(gpu, t, load: .clear, clear: MTLClearColor(red: r, green: g, blue: 0.5, alpha: 1))
            }
            off += try await self.off(gpu, targets)
        }
        return off
    }

    static func blended(_ gpu: GPU, _ targets: [MTLTexture]) async throws -> Int {
        _ = try await swept(gpu, targets)
        for t in targets { try await pass(gpu, t, load: .load, clear: MTLClearColor(), blend: over) }
        return try await off(gpu, targets)
    }
}
