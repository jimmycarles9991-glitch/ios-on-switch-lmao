import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// copy.ce_compressed for zetas (drivers/TegraGM20BMetal/raw/runtime/raw_comp.h): the copy engine copying a compressed
/// Depth32Float or multisampled Depth32Float_Stencil8 zeta (kinds ZF32_2CZ, ZF32_MS4_2CZ, ZF32_X24S8_MS2/MS4_2CSZV) into the
/// shader's depth-test images and back, as the facade does around framebuffer-fetching draws that test depth in the
/// shader. A debug build's switch (switchRawProbeZetaKinds:) gives those memoryless zetas their compressible kinds; the
/// same pass drawn with it off is the reference. PASS: every round's picture with compressed zetas is the reference's
/// byte for byte and zeta_ce_copies moved; INCONCLUSIVE: no switch, the work left the raw path, or no copy went through
/// compressed memory (the facade had none made yet).
///
/// T-v2's run died on the draws of a multisampled case with INVALID_A_A_COMBINATION03 (class error 0x134 at
/// DRAW_VERTEX_ARRAY), as framebuffer_fetch_depth and fetch_routes do, before judging anything. Our context differs from
/// NVIDIA's on two anti-alias controls: 0x15b4 (golden 1, NVIDIA's userspace writes 0; nvk_cmd_draw.c) and
/// SET_ANTI_ALIAS_ENABLE 0x1534 (NVK 1, golden 0). So the single-sample case runs first, as is, and each multisampled
/// case runs with NVIDIA's 0x15b4 around every draw (`<case>_aa=csaa_off`), then, if that killed its command buffer, with
/// 0x1534 cleared instead (aa_off); a case whose arms all failed is named (`<case>_aa=killed`) and the multisampled cases
/// after it are not run. A multisampled case whose words the channel refused or never emitted judges nothing and makes the
/// claim INCONCLUSIVE. Each case is journaled before it runs.
enum X1CompressedDepthProbe {
    static let probe = X1Probe(
        id: "gpu.x1.ce_compressed_depth",
        name: "The copy engine copies compressed shader-tested zetas in and out as plain texels",
        probe: "copy.ce_compressed_depth", risky: true, timeout: 180
    ) { gpu in try await run(gpu) }

    // compressed memory is made off the render thread after the first ask: rounds wait for it, up to `attempts`
    static let size = 64, rounds = 16, attempts = 60
    static let cases: [(String, MTLPixelFormat, Int)] = [
        ("d32f_1x", .depth32Float, 1), ("d32f_4x", .depth32Float, 4), ("d32s8_2x", .depth32Float_stencil8, 2),
        ("d32s8_4x", .depth32Float_stencil8, 4),
    ]

    static func zetaKinds(_ device: MTLDevice, _ on: Bool) -> Bool {
        let selector = NSSelectorFromString("switchRawProbeZetaKinds:")
        guard let object = device as? NSObject, object.responds(to: selector) else { return false }
        _ = object.perform(selector, with: NSNumber(value: on))
        return true
    }

    static let csaa: UInt32 = 0x15b4, antiAliasEnable: UInt32 = 0x1534

    /// The anti-alias controls a case's draws run under: name, words before each draw, words after it
    static let aaArms: [(String, [(UInt32, UInt32)], [(UInt32, UInt32)])] = [
        ("csaa_off", [(csaa, 0)], [(csaa, 1)]), ("aa_off", [(antiAliasEnable, 0)], [(antiAliasEnable, 1)]),
    ]

    /// One case's reference and compressed rounds: (pictures that differ, rounds through compressed memory)
    static func rounds(_ gpu: GPU, _ scene: Scene) async throws -> (Int, Int) {
        _ = zetaKinds(gpu.device, false)
        let reference = try await scene.draw()
        _ = zetaKinds(gpu.device, true)
        var bad = 0, compressed = 0
        for _ in 0..<attempts where compressed < rounds {
            let before = RawFacade.compression(of: gpu.device)["zeta_ce_copies"] ?? 0
            let picture = try await scene.draw()
            if (RawFacade.compression(of: gpu.device)["zeta_ce_copies"] ?? 0) > before {
                compressed += 1
                if picture != reference { bad += 1 }
            }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        return (bad, compressed)
    }

    static func run(_ gpu: GPU) async throws -> ProbeFinding {
        var metrics = Metrics()
        metrics.set("rule", "copy.ce_compressed_depth")
        guard zetaKinds(gpu.device, false) else {
            return ProbeFinding(.inconclusive, metrics, note: "the facade has no zeta probe switch (a release build)")
        }
        defer { _ = zetaKinds(gpu.device, false) }
        let journal = ProbeJournal(probe: "gpu.x1.ce_compressed_depth")
        var wrong: [String: Int] = [:]
        var engaged = 0
        var killed: [String] = [], refused: [String] = []
        var dead: String?
        cases: for (name, format, samples) in cases where gpu.device.supportsTextureSampleCount(samples) {
            let scene = try Scene(gpu: gpu, format: format, samples: samples)
            let arms = samples > 1 ? aaArms : [("as_is", [], [])]
            for (arm, before, after) in arms {
                journal.starting("\(name) \(arm)")
                if !before.isEmpty, let refusal = RawMethods.arm(gpu.device, before: before, after: after) {
                    metrics.set("\(name)_aa", LogLine.sanitized(refusal))
                    refused.append(name)
                    continue cases
                }
                do {
                    let (bad, compressed) = try await rounds(gpu, scene)
                    let emitted = before.isEmpty ? 0 : RawMethods.disarm(gpu.device)
                    if !before.isEmpty && emitted == 0 {
                        metrics.set("\(name)_aa", "\(arm)_not_emitted")
                        refused.append(name)
                        continue cases
                    }
                    wrong[name] = bad
                    metrics.set("\(name)_aa", arm)
                    metrics.set("\(name)_compressed_rounds", compressed)
                    engaged += compressed
                    continue cases
                } catch {
                    if !before.isEmpty { _ = RawMethods.disarm(gpu.device) }
                    killed.append("\(name):\(arm)")
                }
            }
            metrics.set("\(name)_aa", "killed")
            dead = name
            break
        }
        metrics.set("killed_arms", killed.isEmpty ? "-" : killed.joined(separator: ","))
        metrics.set("compressed_rounds", engaged)
        let paths = gpu.paths.paths.keys.map { $0 }
        if engaged == 0 {
            var finding = ProbeFinding.counted(wrong, paths: paths, metrics: metrics)
            finding.verdict = .inconclusive
            finding.note = "no shader-test copy went through a compressed zeta"
            return finding
        }
        var finding = ProbeFinding.counted(wrong, paths: paths, metrics: metrics)
        if finding.verdict == .pass, let dead {
            finding.verdict = .inconclusive
            finding.note = "\(dead)'s draws were killed under every anti-alias arm"
        } else if finding.verdict == .pass, !refused.isEmpty {
            finding.verdict = .inconclusive
            finding.note = "no anti-alias words reached the draws of \(refused.joined(separator: ","))"
        }
        return finding
    }

    /// FramebufferFetchDepthScene's draws over a memoryless zeta: fetching draws test depth in the shader, the plain one
    /// between them on the fixed-function test, so the zeta goes through the copy engine both ways
    struct Scene {
        let gpu: GPU
        let target: MTLTexture, resolved: MTLTexture?, depth: MTLTexture
        let fetch: MTLRenderPipelineState, plain: MTLRenderPipelineState, less: MTLDepthStencilState
        let stencil: Bool

        init(gpu: GPU, format: MTLPixelFormat, samples: Int) throws {
            self.gpu = gpu
            stencil = format == .depth32Float_stencil8
            depth = try gpu.texture(format, size, size, usage: [.renderTarget], samples: samples, storage: .memoryless)
            target = try gpu.texture(.rgba8Unorm, size, size, samples: samples)
            resolved = samples > 1 ? try gpu.texture(.rgba8Unorm, size, size) : nil
            fetch = try gpu.render(
                vertex: "fetch_depth_vertex", fragment: "fetch_add_fragment", format: .rgba8Unorm, samples: samples,
                depth: format, stencil: stencil ? format : .invalid)
            plain = try gpu.render(
                vertex: "fetch_depth_vertex", fragment: "solid_fragment", format: .rgba8Unorm, samples: samples,
                depth: format, stencil: stencil ? format : .invalid)
            let descriptor = MTLDepthStencilDescriptor()
            descriptor.depthCompareFunction = .less
            descriptor.isDepthWriteEnabled = true
            less = gpu.device.makeDepthStencilState(descriptor: descriptor)!
        }

        func draw() async throws -> [UInt8] {
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = resolved == nil ? .store : .multisampleResolve
                pass.colorAttachments[0].resolveTexture = resolved
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = 1
                pass.depthAttachment.storeAction = .dontCare
                if stencil {
                    pass.stencilAttachment.texture = depth
                    pass.stencilAttachment.loadAction = .clear
                    pass.stencilAttachment.storeAction = .dontCare
                }
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setDepthStencilState(less)
                for (state, z, colour) in [
                    (fetch, Float(0.75), SIMD4<Float>(0.125, 0, 0, 1)), (plain, 0.5, SIMD4(0.25, 0.25, 0.25, 1)),
                    (fetch, 0.625, SIMD4(0, 0.5, 0, 1)), (fetch, 0.25, SIMD4(0, 0, 0.125, 1)),
                    (plain, 0.375, SIMD4(0.5, 0.5, 0.5, 1)), (fetch, 0.125, SIMD4(0, 0.125, 0, 1)),
                ] {
                    encoder.setRenderPipelineState(state)
                    var depth = z
                    var colour = colour
                    encoder.setVertexBytes(&depth, length: 4, index: 0)
                    encoder.setFragmentBytes(&colour, length: 16, index: 0)
                    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                }
                encoder.endEncoding()
            }
            return try await gpu.read(resolved ?? target, bytesPerPixel: 4)
        }
    }
}
