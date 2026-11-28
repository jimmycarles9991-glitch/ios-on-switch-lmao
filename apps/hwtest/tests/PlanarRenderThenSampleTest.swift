import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// A two-plane YCbCr texture written and sampled in one command buffer gives exactly the texels the same work split
/// over command buffers gives, each committed and waited for (64x64, 502 over '422v', 500 and 540 over '420v', the 709
/// matrix, both planes scribbled; PlanarRenderThenSampleCases): a clear and a scissored draw, then a sample (render); a
/// loaded pass redrawing an odd-edged part, then a sample (load); a sample, then the draw, the sample seeing the old
/// planes (sample_first); the draw, a sample, a clear of other values, a sample (twice); the draw, then a kernel sampling
/// it (kernel); bytes copied into the luma plane of a ShaderRead-only texture, then a sample (copy). Every sample after a
/// write differs from the planes before it, so neither side can be stale alike. The split is each run's own reference;
/// that the two agree, tolerance 0, is an M4 Max's (c38-audit/m2v-planar-plan/probe/planarsample.mm, and these cases on
/// the same Mac); render's (4,4) and (32,32) for 502 and 500 are the M4 Max's within 0.001 (probe/out.txt). Fields:
/// <fmt>_same_cb_off= channels that differ summed over the arms, <fmt>_<arm>_off= where not 0, <fmt>_unseen= arms whose
/// sample after a write equals the planes before it, <fmt>_spot_off=, paths=, failed_cbs=.
struct PlanarRenderThenSampleTest: HWTest {
    let id = "gpu.gm20b.planar_render_then_sample"
    let name = "A YCbCr texture written then sampled in one command buffer samples as the same work split, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        return await Self.outcome(gpu)
    }

    static func outcome(_ gpu: GPU) async -> Outcome {
        typealias Cases = PlanarRenderThenSampleCases
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var kept: Set<String> = ["failed_cbs"]
        let library: MTLLibrary
        do {
            library = try await Cases.library(gpu)
        } catch {
            return .fail("the library: \(GPU.describe(error))", proof: gpu.proof)
        }
        for f in Cases.formats {
            var sameCb = 0
            var spotOff: Int?
            var unseen: [String] = []
            var errors: [String] = []
            do {
                let states = try await Cases.states(gpu, library, f)
                for arm in Cases.arms {
                    do {
                        let r = try await Cases.exercise(gpu, states, f, arm)
                        sameCb += r.off
                        if r.off != 0 {
                            metrics.set("\(f.key)_\(arm.key)_off", r.off)
                            metrics.set("\(f.key)_\(arm.key)_first", r.first)
                        }
                        if r.unseen { unseen.append(arm.key) }
                        if let off = r.spotOff { spotOff = off }
                    } catch {
                        errors.append("\(arm.key): \(GPU.describe(error))")
                    }
                }
            } catch {
                errors.append("states: \(GPU.describe(error))")
            }
            metrics.set("\(f.key)_same_cb_off", sameCb)
            kept.insert("\(f.key)_same_cb_off")
            if !unseen.isEmpty { metrics.set("\(f.key)_unseen", unseen.joined(separator: ",")) }
            checks.append(("\(f.key): one command buffer samples what the split does in every arm", sameCb == 0))
            checks.append(("\(f.key): every sample after a write sees it", unseen.isEmpty))
            checks += errors.map { ("\(f.key) \($0)", false) }
            if !f.spots.isEmpty {
                metrics.set("\(f.key)_spot_off", spotOff ?? -1)
                kept.insert("\(f.key)_spot_off")
                checks.append(("\(f.key): render's texels an M4 Max's within 0.001", spotOff == 0))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: kept)
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
