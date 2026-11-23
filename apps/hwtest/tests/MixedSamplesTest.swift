import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A one-sample colour beside a 4x depth (MixedSamplesCases), which decides two inferred fields of the driver's mixed
/// samples (mixed_samples.h): SET_TIR_CONTROL REDUCE_COVERAGE's meaning and whether the zeta follows
/// SET_ANTI_ALIAS_RASTER under TIR. Expected values are an M4 Max's (mixprobe.mm, analyse.py; this scene measured
/// again in a fresh process after one warm-up mixed pass): every pixel is rasterised at the depth's 4 samples; the depth
/// is 0.5 after in every covered, passing sample under either state; the colour is white where ANY covered sample passes
/// under a state of 1 raster sample, where sample 0 does under a state of 4, and never another value than 0 or 255.
/// mixed_r<n>_white is masks 0..15 in order (1 white, 0 not, m some pixels), with mixed_r<n>_mask_<m>_white only where
/// a mask is off; mixed_r<n>_depth_wrong_samples the samples whose depth after was wrong, bit s for sample s. Over a
/// triangle and hashed masks, mixed_edge_r<n>_match pixels follow the state's rule (1024/1024 on the Mac),
/// _swapped the other rule's, _depth the pixels whose written samples are the covered and passing ones (reported; the
/// Mac 1023 under analyse.py's coverage model) and _rows each row's edge pixels (MixedSamplesCases.edge's codes).
struct MixedSamplesTest: HWTest {
    let id = "gpu.gm20b.mixed_samples"
    let name = "A one-sample colour beside a 4x depth tests, writes and resolves its samples as Metal's, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static let expectedWhite = [1: "0111111111111111", 4: "0101010101010101"]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        return await Self.outcome(gpu)
    }

    static func outcome(_ gpu: GPU) async -> Outcome {
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var grey = 0, fillOff = 0
        var kept: Set<String> = ["failed_cbs"]
        do {
            let scene = try MixedSamplesCases.Scene(gpu)
            _ = try await scene.run(mask: 0, raster: 1)
            for raster in [1, 4] {
                let key = "mixed_r\(raster)"
                do {
                    var runs: [MixedSamplesCases.Run] = []
                    for mask in 0 ..< 16 { runs.append(try await scene.run(mask: UInt32(mask), raster: raster)) }
                    let u = MixedSamplesCases.uniform(runs, raster: raster)
                    grey += u.grey
                    metrics.set(key + "_white", u.white)
                    for (mask, shown) in u.off {
                        if let bit = Int(shown) {
                            metrics.set("\(key)_mask_\(mask)_white", bit)
                        } else {
                            metrics.set("\(key)_mask_\(mask)_white", shown)
                        }
                    }
                    metrics.set(key + "_depth_ok", u.wrongSamples == 0)
                    metrics.set(key + "_depth_wrong_samples", String(u.wrongSamples, radix: 16))
                    kept.formUnion([key + "_white", key + "_depth_ok", key + "_depth_wrong_samples"])
                    checks.append(("raster \(raster): white as Metal's", u.white == expectedWhite[raster]))
                    checks.append(("raster \(raster): depth 0.5 in the passing samples", u.wrongSamples == 0))
                } catch {
                    checks.append(("raster \(raster) masks: \(GPU.describe(error))", false))
                }
                do {
                    let run = try await scene.run(mask: MixedSamplesCases.hashed, raster: raster, triangle: true)
                    let e = MixedSamplesCases.edge(run, raster: raster)
                    grey += e.grey
                    fillOff += e.fillOff
                    let edge = "mixed_edge_r\(raster)"
                    metrics.set(edge + "_match", e.match)
                    metrics.set(edge + "_swapped", e.swapped)
                    metrics.set(edge + "_depth", e.depthMatch)
                    metrics.set(edge + "_rows", e.rows.joined(separator: "/"))
                    kept.formUnion([edge + "_match", edge + "_swapped", edge + "_depth", edge + "_rows"])
                    let pixels = MixedSamplesCases.side * MixedSamplesCases.side
                    checks.append(("raster \(raster): triangle colour by the state's rule", e.match == pixels))
                } catch {
                    checks.append(("raster \(raster) triangle: \(GPU.describe(error))", false))
                }
            }
        } catch {
            checks.append(("the scene: \(GPU.describe(error))", false))
        }
        metrics.set("mixed_grey", grey)
        metrics.set("mixed_fill_off", fillOff)
        checks.append(("no colour value but 0 and 255", grey == 0))
        checks.append(("the 4x fill wrote each sample its mask's bit", fillOff == 0))
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: kept.union(["mixed_grey", "mixed_fill_off"]))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
