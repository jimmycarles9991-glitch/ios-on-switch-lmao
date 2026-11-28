import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Alpha to coverage and alpha to one on the driver (SET_ANTI_ALIAS_ALPHA_CONTROL, caps 3D-E5): white drawn at alpha
/// (x + 0.5) / 256 over black into a 4x MSAA BGRA8 target, resolved. Metal's rule is that each sample is covered or not,
/// so a resolved pixel holds k/4 of white for k covered samples (a code within one of 255 k / 4). A 2x2 block's 16
/// samples follow alpha within two: per-pixel rounding (no dither) is off by half a sample a pixel at most, and a 2x2
/// dither (SET_ALPHA_TO_COVERAGE_DITHER_CONTROL) by less. Alpha to one writes alpha 1 to the covered samples, so a
/// resolved alpha is k/4 too. PASS: both on the driver, every pixel a whole number of samples, every block within two
/// samples of alpha, and no block more than one sample below the block to its left.
struct AlphaToCoverageTest: HWTest {
    let id = "gpu.gm20b.alpha_to_coverage"
    let name = "Alpha to coverage and alpha to one: whole samples, coverage following alpha, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let width = 256, height = 8, samples = 4

    struct Reading {
        var notWhole = 0, offAlpha = 0, falling = 0, alphaNotWhole = 0
        var worstBlock = 0.0
        var path = ""
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        guard gpu.device.supportsTextureSampleCount(Self.samples) else { return .skip("no 4x MSAA") }
        var metrics = Metrics()
        var wrong: [String] = []
        var plainBytes: [UInt8] = []
        for (name, toOne) in [("coverage", false), ("coverage_one", true)] {
            let (r, bytes) = try await reading(
                gpu, fragment: "a2c_ramp_fragment", alphaToCoverage: true, alphaToOne: toOne
            )
            if !toOne { plainBytes = bytes }
            metrics.set("\(name)_path", r.path)
            metrics.set("\(name)_not_whole", r.notWhole)
            metrics.set("\(name)_off_alpha", r.offAlpha)
            metrics.set("\(name)_falling", r.falling)
            metrics.set("\(name)_worst_block", r.worstBlock, digits: 3)
            if toOne { metrics.set("\(name)_alpha_not_whole", r.alphaNotWhole) }
            if r.path != "gm20b" { wrong.append("\(name)_path") }
            if r.notWhole > 0 || r.offAlpha > 0 || r.falling > 0 || r.alphaNotWhole > 0 { wrong.append(name) }
        }
        let (fetchCovered, fetchCoveredBytes) = try await reading(
            gpu, fragment: "a2c_fetch_ramp_fragment", alphaToCoverage: true, alphaToOne: false
        )
        let (fetchPlain, fetchPlainBytes) = try await reading(
            gpu, fragment: "a2c_fetch_ramp_fragment", alphaToCoverage: false, alphaToOne: false
        )
        var diffPlain = 0
        var diffUnflagged = 0
        for p in 0 ..< (Self.width * Self.height) {
            let g = p * 4 + 1
            if fetchCoveredBytes[g] != plainBytes[g] { diffPlain += 1 }
            if fetchCoveredBytes[g] != fetchPlainBytes[g] { diffUnflagged += 1 }
        }
        metrics.set("fetch_coverage_path", fetchCovered.path)
        metrics.set("fetch_plain_path", fetchPlain.path)
        metrics.set("fetch_coverage_diff_plain", diffPlain)
        metrics.set("fetch_coverage_diff_unflagged", diffUnflagged)
        if fetchCovered.path != "gm20b" { wrong.append("fetch_coverage_path") }
        if fetchPlain.path != "gm20b" { wrong.append("fetch_plain_path") }
        if diffPlain > 0 { wrong.append("fetch_coverage_match") }
        if diffUnflagged == 0 { wrong.append("fetch_coverage_distinct") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    func reading(
        _ gpu: GPU, fragment: String, alphaToCoverage: Bool, alphaToOne: Bool
    ) async throws -> (Reading, [UInt8]) {
        let state = try gpu.render(fragment: fragment, format: .bgra8Unorm, samples: Self.samples) { d in
            d.isAlphaToCoverageEnabled = alphaToCoverage
            d.isAlphaToOneEnabled = alphaToOne
        }
        let ms = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.renderTarget], samples: Self.samples)
        let resolved = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.renderTarget, .shaderRead])
        var r = Reading()
        r.path = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = ms
            pass.colorAttachments[0].resolveTexture = resolved
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].storeAction = .multisampleResolve
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            var width = Float(Self.width)
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setFragmentBytes(&width, length: 4, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
        }.path
        let bytes = try await gpu.read(resolved, bytesPerPixel: 4)
        func samplesAt(_ x: Int, _ y: Int, channel: Int) -> Double {
            Double(bytes[(y * Self.width + x) * 4 + channel]) * Double(Self.samples) / 255
        }
        var last = -1.0
        for by in stride(from: 0, to: Self.height, by: 2) {
            last = -1
            for bx in stride(from: 0, to: Self.width, by: 2) {
                var covered = 0.0
                for (ox, oy) in [(0, 0), (1, 0), (0, 1), (1, 1)] {
                    let k = samplesAt(bx + ox, by + oy, channel: 1)
                    if abs(k - k.rounded()) * 255 / Double(Self.samples) > 1 { r.notWhole += 1 }
                    if alphaToOne, abs(samplesAt(bx + ox, by + oy, channel: 3) - k) * 255 / Double(Self.samples) > 1 {
                        r.alphaNotWhole += 1
                    }
                    covered += k.rounded()
                }
                let alpha = (Double(bx) + 1) / Double(Self.width)
                let miss = abs(covered - alpha * Double(4 * Self.samples))
                r.worstBlock = max(r.worstBlock, miss)
                if miss > 2 { r.offAlpha += 1 }
                if covered < last - 1 { r.falling += 1 }
                last = covered
            }
        }
        return (r, bytes)
    }
}
