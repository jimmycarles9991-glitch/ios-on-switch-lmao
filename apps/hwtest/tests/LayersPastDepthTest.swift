import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// What a layered pass of 4 tests against past a loaded depth/stencil attachment of 2 layers (LayersPastDepthCases),
/// filled with depth 0.25 and stencil 5. Expected answers are an M4 Max's (layerprobe.mm, layers.txt; this scene
/// measured): a full quad at z 1.0 passes in layers 0-1 under Greater, GreaterEqual and NotEqual; in layers 2-3 a
/// Depth32Float or Depth32Float_Stencil8 tests against a value above 1.0 (Less, LessEqual, NotEqual pass) and a
/// Depth16Unorm against exactly 1.0 (LessEqual, Equal, GreaterEqual pass); a quad at z 0.5 under stencil Equal passes
/// in layers 0-1 for reference 5 alone and in layers 2-3 for 255 alone. past_<format> is each layer's answers, layers
/// 0 to 3, each in the order Less, LessEqual, Equal, Greater, GreaterEqual, NotEqual (1 drawn, 0 not, m some texels);
/// past_stencil_<format> the same over references 0, 1, 5, 9, 128, 255; past_<format>_<compare>_l<l> and
/// past_stencil_<format>_ref<r>_l<l> only where a layer is off.
/// Reported, not judged: a Depth32Float loaded DontCare, filled with the opposite of the pass's clear depth, a quad at z
/// 0.5 (dontcare_clear0 and _clear1: each compare's four layers, in the order above). An M4 Max tests every layer, its
/// own included, against the clear depth: clear 0 0000/0000/0000/1111/1111/1111, clear 1 1111/1111/0000/0000/0000/1111.
/// dontcare_gpu_ms and dontcare_clear_gpu_ms are one 1280x720 pass's GPU time loading the depth DontCare and Clear (the
/// least of five).
struct LayersPastDepthTest: HWTest {
    let id = "gpu.gm20b.layers_past_depth"
    let name = "A layered pass past a loaded depth/stencil attachment's layers tests against Metal's values, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static let ownLayers = "000111"
    static let pastLayers = ["d32f": "110001", "d16": "011010", "d32fs8": "110001"]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        return await Self.outcome(gpu)
    }

    /// Per layer, each run's answer for it in run order
    static func byLayer(_ answers: [String]) -> [String] {
        (0 ..< 4).map { l in String(answers.map { Array($0)[l] }) }
    }

    static func offFields(
        _ prefix: String, _ names: [String], _ layers: [String], _ expected: [String]
    )
        -> Metrics
    {
        var metrics = Metrics()
        for l in 0 ..< 4 {
            for (i, (got, want)) in zip(layers[l], expected[l]).enumerated() where got != want {
                let key = "\(prefix)_\(names[i])_l\(l)"
                if let bit = Int(String(got)) { metrics.set(key, bit) } else { metrics.set(key, String(got)) }
            }
        }
        return metrics
    }

    static func outcome(_ gpu: GPU) async -> Outcome {
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var kept: Set<String> = ["failed_cbs"]
        let compares = LayersPastDepthCases.compares
        for f in [LayersPastDepthCases.Format.d32f, .d16, .d32fs8] {
            do {
                let state = try LayersPastDepthCases.state(gpu, f)
                var answers: [String] = []
                for c in compares {
                    answers.append(try await LayersPastDepthCases.drawn(gpu, f, state, .init(depthCompare: c.function)))
                }
                let layers = byLayer(answers)
                let past = pastLayers[f.key]!
                let expected = [ownLayers, ownLayers, past, past]
                metrics.set("past_\(f.key)", layers.joined(separator: "/"))
                kept.insert("past_\(f.key)")
                metrics.merge(offFields("past_\(f.key)", compares.map(\.key), layers, expected))
                checks.append(("\(f.key): layers 0-1 against 0.25", Array(layers[0 ..< 2]) == [ownLayers, ownLayers]))
                checks.append(("\(f.key): layers 2-3 as Metal's (\(past))", Array(layers[2 ..< 4]) == [past, past]))
            } catch {
                checks.append(("\(f.key): \(GPU.describe(error))", false))
            }
        }
        let references = LayersPastDepthCases.references
        for f in [LayersPastDepthCases.Format.s8, .d32fs8] {
            do {
                let state = try LayersPastDepthCases.state(gpu, f)
                var answers: [String] = []
                for r in references {
                    answers.append(
                        try await LayersPastDepthCases.drawn(
                            gpu, f, state, .init(stencilCompare: .equal, reference: r, z: 0.5)
                        )
                    )
                }
                let layers = byLayer(answers)
                let own = String(references.map { $0 == 5 ? "1" : "0" }.joined())
                let past = String(references.map { $0 == 255 ? "1" : "0" }.joined())
                metrics.set("past_stencil_\(f.key)", layers.joined(separator: "/"))
                kept.insert("past_stencil_\(f.key)")
                metrics.merge(
                    offFields("past_stencil_\(f.key)", references.map { "ref\($0)" }, layers, [own, own, past, past])
                )
                checks.append(("\(f.key) stencil: layers 0-1 hold 5", Array(layers[0 ..< 2]) == [own, own]))
                checks.append(("\(f.key) stencil: layers 2-3 hold 0xff", Array(layers[2 ..< 4]) == [past, past]))
            } catch {
                checks.append(("\(f.key) stencil: \(GPU.describe(error))", false))
            }
        }
        for clear in [0, 1] {
            let key = "dontcare_clear\(clear)"
            do {
                let state = try LayersPastDepthCases.state(gpu, .d32f)
                var answers: [String] = []
                for c in compares {
                    let run = LayersPastDepthCases.Run(
                        depthCompare: c.function, z: 0.5, load: .dontCare, filledDepth: Double(1 - clear),
                        clearDepth: Double(clear)
                    )
                    answers.append(try await LayersPastDepthCases.drawn(gpu, .d32f, state, run))
                }
                metrics.set(key, answers.joined(separator: "/"))
            } catch {
                metrics.set(key, "error_" + GPU.describe(error).prefix(80))
            }
            kept.insert(key)
        }
        do {
            let dontCare = try await LayersPastDepthCases.passMilliseconds(gpu, load: .dontCare)
            let cleared = try await LayersPastDepthCases.passMilliseconds(gpu, load: .clear)
            metrics.set("dontcare_gpu_ms", dontCare, digits: 3)
            metrics.set("dontcare_clear_gpu_ms", cleared, digits: 3)
        } catch {
            metrics.set("dontcare_gpu_ms", "error_" + GPU.describe(error).prefix(80))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: kept.union(["dontcare_gpu_ms", "dontcare_clear_gpu_ms"]))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
