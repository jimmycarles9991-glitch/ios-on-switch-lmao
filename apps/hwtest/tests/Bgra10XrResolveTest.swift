import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// BGRA10_XR (552) multisampled resolves: a 64x64 target at 2x and 4x, each sample drawn its own codes
/// (standin_sample_values, value (c - 384) / 510; colour codes 0...1023, alpha 384...894; rows 0-15 ties: 2x c and c +
/// 1, 4x four codes summing to 4k + 2; rows 16-63 random), MultisampleResolve into a one-sample BGRA10_XR read as
/// 16-bit fields >> 6. Every colour channel must be floor(sum / n + 0.5), tolerance 0, and alpha within one code of
/// floor((sum - 1) / n + 0.5), ties down (the spec's rules from resolvecodesoracle.mm: an M4 Max rounds alpha ties down
/// in 88% / 98% of them; this scene on an M4 Max: 0 colour channels off, alpha at most one code off, 845 / 447 alpha
/// codes off the ties-down rule at 2x / 4x). Per count x<n>_colour_off channels off, x<n>_colour_max, x<n>_alpha_off
/// alpha codes not the ties-down rule's, x<n>_alpha_max; every command buffer raw, failed_cbs=0.
struct Bgra10XrResolveTest: HWTest {
    let id = "gpu.gm20b.bgra10xr_resolve"
    let name = "BGRA10_XR resolves to its colour codes' mean rounded half up and alpha within one code, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct Judged {
        var colourOff = 0
        var colourWorst = 0
        var alphaOff = 0
        var alphaWorst = 0
    }

    static let side = 64, tieTexels = 16 * 64
    static let format = MTLPixelFormat.bgra10_xr

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for samples in [2, 4] {
            let key = "x\(samples)"
            do {
                let judged = try await Self.resolved(gpu, samples: samples)
                metrics.set(key + "_colour_off", judged.colourOff)
                metrics.set(key + "_colour_max", judged.colourWorst)
                metrics.set(key + "_alpha_off", judged.alphaOff)
                metrics.set(key + "_alpha_max", judged.alphaWorst)
                checks.append(("\(key): every colour code the codes' mean rounded half up", judged.colourOff == 0))
                checks.append(("\(key): every alpha within one code of its ties-down mean", judged.alphaWorst <= 1))
            } catch {
                checks.append(("\(key): \(GPU.describe(error))", false))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func codes(samples: Int) -> [[Int]] {
        var state = UInt64(format.rawValue) * 131 + UInt64(samples)
        let pick = { (range: ClosedRange<Int>) in
            range.lowerBound + Int(WordMipmapCases.splitmix(&state) % UInt64(range.count))
        }
        let patterns = [[0, 0, 1, 1], [-1, 1, 1, 1], [2, 0, 0, 0], [3, -1, 0, 0], [-2, 2, 1, 1]]
        var codes = [[Int]](repeating: [0, 0, 0, 0], count: side * side * samples)
        for texel in 0 ..< side * side {
            for channel in 0 ..< 4 {
                let range = channel == 3 ? 384 ... 894 : 0 ... 1023
                var drawn: [Int]
                if texel < tieTexels && samples == 2 {
                    let c = pick(range.lowerBound ... range.upperBound - 1)
                    drawn = pick(0 ... 1) == 0 ? [c, c + 1] : [c + 1, c]
                } else if texel < tieTexels {
                    let k = pick(range.lowerBound + 2 ... range.upperBound - 3)
                    let pattern = patterns[pick(0 ... patterns.count - 1)]
                    let turn = pick(0 ... 3)
                    drawn = (0 ..< 4).map { k + pattern[($0 + turn) % 4] }
                } else {
                    drawn = (0 ..< samples).map { _ in pick(range) }
                }
                for (sample, code) in drawn.enumerated() { codes[texel * samples + sample][channel] = code }
            }
        }
        return codes
    }

    static func resolved(_ gpu: GPU, samples: Int) async throws -> Judged {
        let codes = codes(samples: samples)
        let values = codes.map { SIMD4<Float>($0.map { Float($0 - 384) / 510 }) }
        let drawn = try gpu.texture(format, side, side, usage: [.renderTarget], samples: samples)
        let one = try gpu.texture(format, side, side)
        try await StandInResolveCases.drawSamples(gpu, values, into: drawn, resolve: one, store: .multisampleResolve)
        let bytes = try await gpu.read(one, bytesPerPixel: 8)
        var judged = Judged()
        for texel in 0 ..< side * side {
            for channel in 0 ..< 4 {
                let field = channel == 3 ? 3 : 2 - channel
                let got = Int(bytes.loaded(as: UInt16.self, at: texel * 4 + field) >> 6)
                let sum = (0 ..< samples).reduce(0) { $0 + codes[texel * samples + $1][channel] }
                let tiesDown = channel == 3 ? 1 : 0
                let want = Int((Double(sum - tiesDown) / Double(samples) + 0.5).rounded(.down))
                let off = abs(got - want)
                if channel == 3 {
                    judged.alphaOff += off == 0 ? 0 : 1
                    judged.alphaWorst = max(judged.alphaWorst, off)
                } else {
                    judged.colourOff += off == 0 ? 0 : 1
                    judged.colourWorst = max(judged.colourWorst, off)
                }
            }
        }
        return judged
    }
}
