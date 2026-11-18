import Foundation
import HWTestCore
import Metal

/// New private textures read before anything writes them, as Apple's GPUs answer: a texture Metal keeps losslessly
/// compressed (allowGPUOptimizedContents, a shorter side of 16 or more, no shader-write, format-view or atomic usage) reads
/// one fixed texel in each level down to a shorter side of 16 and zero below; every other one reads zero. Each case is read
/// back by the blit engine level by level, one is sampled by a draw, and one is cleared whole by its first pass. On the raw
/// facade, fresh_filled, fresh_cleared and fresh_early_reads are the deltas of its switchRawFresh counters (a debug build's).
struct FreshTextureScene: GPUScene {
    let id = "gpu.texture.fresh"
    let name = "New private textures read before anything writes them"

    struct Case {
        let label: String
        let format: MTLPixelFormat
        let texel: [UInt8]
        var width = 64, height = 64, levels = 1
        var usage: MTLTextureUsage = [.renderTarget, .shaderRead]
        var optimized = true
    }

    static let magenta: [UInt8] = [0xff, 0x00, 0xff, 0xff]
    static let cases: [Case] = [
        Case(label: "bgra8", format: .bgra8Unorm, texel: magenta),
        Case(
            label: "bgra8_write", format: .bgra8Unorm, texel: magenta,
            usage: [.renderTarget, .shaderRead, .shaderWrite]),
        Case(
            label: "bgra8_view", format: .bgra8Unorm, texel: magenta,
            usage: [.renderTarget, .shaderRead, .pixelFormatView]),
        Case(label: "bgra8_plain", format: .bgra8Unorm, texel: magenta, optimized: false),
        Case(label: "bgra8_narrow", format: .bgra8Unorm, texel: magenta, width: 15),
        Case(label: "bgra8_bar", format: .bgra8Unorm, texel: magenta, width: 1280, height: 108),
        Case(label: "bgra8_chain", format: .bgra8Unorm, texel: magenta, levels: 7),
        Case(label: "rgba8_sampled", format: .rgba8Unorm, texel: magenta, usage: [.shaderRead]),
        Case(label: "rgba16f", format: .rgba16Float, texel: [0xff, 0xff, 0, 0, 0xff, 0xff, 0xff, 0xff]),
        Case(
            label: "rgba16f_glass", format: .rgba16Float, texel: [0xff, 0xff, 0, 0, 0xff, 0xff, 0xff, 0xff],
            width: 832, height: 1408),
        Case(
            label: "rgba16f_write", format: .rgba16Float, texel: [0xff, 0xff, 0, 0, 0xff, 0xff, 0xff, 0xff],
            usage: [.renderTarget, .shaderRead, .shaderWrite]),
        Case(label: "r32f", format: .r32Float, texel: [0xff, 0xff, 0xff, 0xff]),
    ]

    static func patternedLevels(_ c: Case) -> Int {
        let plain: MTLTextureUsage = [.shaderWrite, .pixelFormatView, .shaderAtomic]
        guard c.optimized, !c.usage.isEmpty, c.usage.isDisjoint(with: plain), min(c.width, c.height) >= 16 else {
            return 0
        }
        return (0..<c.levels).prefix { min(max(1, c.width >> $0), max(1, c.height >> $0)) >= 16 }.count
    }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let before = Self.counters(gpu.device)
        for c in Self.cases {
            let texture = try Self.make(gpu, c)
            let patterned = Self.patternedLevels(c)
            for level in 0..<c.levels {
                let texels = max(1, c.width >> level) * max(1, c.height >> level)
                let texel = level < patterned ? c.texel : [UInt8](repeating: 0, count: c.texel.count)
                let got = try await gpu.read(texture, bytesPerPixel: c.texel.count, level: level)
                let wrong = Self.wrongTexels(got, texel)
                expect(
                    gpu, &result, wrong == 0,
                    "\(c.label) level \(level): \(wrong) of \(texels) texels are not \(Self.hex(texel))")
            }
        }
        try await sampled(gpu, &result)
        try await cleared(gpu, &result)
        let after = Self.counters(gpu.device)
        for key in ["filled", "cleared", "early_reads"] where after[key] != nil {
            result.metrics.set("fresh_" + key, (after[key] ?? 0) - (before[key] ?? 0))
        }
        if after["early_reads"] != nil {
            result.expect((after["early_reads"] ?? 0) > (before["early_reads"] ?? 0), "no early read was counted")
        }
        return result
    }

    /// A fresh compressed texture sampled by a draw reads its pattern.
    private func sampled(_ gpu: GPU, _ result: inout SceneResult) async throws {
        let source = try gpu.texture(.bgra8Unorm, 64, 64)
        let target = try gpu.texture(.bgra8Unorm, 64, 64)
        let pipeline = try gpu.render(vertex: "textured_vertex", fragment: "sampled_fragment", format: .bgra8Unorm)
        let samplerDescriptor = MTLSamplerDescriptor()
        guard let sampler = gpu.device.makeSamplerState(descriptor: samplerDescriptor) else {
            result.expect(false, "the nearest sampler returned nil")
            return
        }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            var corners = GPU.fullScreen
            var uvs: [SIMD2<Float>] = [SIMD2(0, 1), SIMD2(1, 1), SIMD2(0, 0), SIMD2(1, 0)]
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setVertexBytes(&uvs, length: MemoryLayout<SIMD2<Float>>.stride * 4, index: 1)
            encoder.setFragmentTexture(source, index: 0)
            encoder.setFragmentSamplerState(sampler, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        let wrong = Self.wrongTexels(try await gpu.read(target, bytesPerPixel: 4), Self.magenta)
        expect(gpu, &result, wrong == 0, "sampled: \(wrong) of 4096 texels are not the pattern")
    }

    /// A fresh texture whose first pass clears it whole reads the clear colour.
    private func cleared(_ gpu: GPU, _ result: inout SceneResult) async throws {
        let target = try gpu.texture(.bgra8Unorm, 64, 64)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 1, alpha: 1)
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        let wrong = Self.wrongTexels(try await gpu.read(target, bytesPerPixel: 4), [0xff, 0, 0, 0xff])
        result.expect(wrong == 0, "cleared: \(wrong) of 4096 texels are not the clear colour")
    }

    /// m2v (frozen, to be deleted) is measured, not judged: its misses are notes
    private func expect(_ gpu: GPU, _ result: inout SceneResult, _ condition: Bool, _ problem: String) {
        if RawFacade.kind(of: gpu.device) == "m2v" {
            if !condition { result.notes.append(problem) }
            return
        }
        result.expect(condition, problem)
    }

    static func make(_ gpu: GPU, _ c: Case) throws -> MTLTexture {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: c.format, width: c.width, height: c.height, mipmapped: c.levels > 1)
        descriptor.mipmapLevelCount = c.levels
        descriptor.usage = c.usage
        descriptor.storageMode = .private
        descriptor.allowGPUOptimizedContents = c.optimized
        guard let texture = gpu.device.makeTexture(descriptor: descriptor) else {
            throw TestFailure("makeTexture(\(c.label)) returned nil")
        }
        return texture
    }

    static func wrongTexels(_ bytes: [UInt8], _ texel: [UInt8]) -> Int {
        stride(from: 0, to: bytes.count, by: texel.count).filter { !bytes[$0..<$0 + texel.count].elementsEqual(texel) }
            .count
    }

    static func hex(_ bytes: [UInt8]) -> String { bytes.map { String(format: "%02x", $0) }.joined() }

    static func counters(_ device: MTLDevice) -> [String: Int] {
        (RawFacade.ask(device, "switchRawFresh") as? [String: NSNumber])?.mapValues(\.intValue) ?? [:]
    }
}
