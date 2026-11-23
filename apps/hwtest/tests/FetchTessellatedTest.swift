import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Tessellated draws whose fragment function reads the colour it writes keep Metal's order on the driver: patch order,
/// then instance order, and inside a patch an order that never tears, where the state's storage form (a depth-format
/// state's per-test-class variant included) draws each patch whole inside the pixel interlock. Each arm
/// (FetchTessellatedCases) draws quad patches over a 64x64 target cleared to (0, 0, 0, 1) whose fragment writes
/// R = dst.r / 2 + c, G = dst.g + step, so each of the 3600 interior pixels' codes name the order its fragments ran in or a
/// tear (G short of a step a fragment). The expected counts of first_then_second / second_then_first are an M4 Max's,
/// the same image in each of ten runs with validation on and off (c38-audit/m2v-tess-fetch/tess_probe.mm): a one-patch
/// fold at factor 2 2759 / 841 (u and v, direct, indirect, as an indirect command buffer's patch command, BGR5A1Unorm),
/// 2984 / 616 (u at 4), 2504 / 1096 (v at 4), 3288 / 312 (u at 8); two flat patches and two instances of one 3600 / 0;
/// six flat patches every pixel R 65, G 48 (tess_six_ordered 3600); tess_depth_u2, whose fragment exports depth 0.25
/// under Less with writes, keeps the earlier fragment alone (R 96 or 32, G one step), 2759 / 841; 4x, every sample
/// shaded on its own (samples_distinct 3600), 2730 / 870 at samples 0 and 1 and 2788 / 812 at 2 and 3 (these cases on
/// the same M4 Max, the image every run alike; the probe printed only each sample's codes; the ms4 counts are of
/// samples). Per arm <arm>_order= apple (those counts), stable (the same image every run, other counts: GM20B's
/// tessellator orders a patch's triangles otherwise) or torn (runs differ, or a pixel read a stale colour); torn, a pixel
/// in neither order, a flat arm off Metal's order or a failed command buffer fails. A command buffer failing
/// MTLCommandBufferErrorInternal is a state with no storage form for its pass (gm20b:draw.fetch_patches):
/// <arm>_blocked=fetch_patches, and the arm fails. Fields per arm _first_then_second, _second_then_first, _torn, _other,
/// _stable, _error, _order; readback_paths=, failed_cbs=.
struct FetchTessellatedTest: HWTest {
    typealias Cases = FetchTessellatedCases

    let id = "gpu.gm20b.fetch_tessellated"
    let name = "Tessellated draws reading the colour they write keep patch, instance and in-patch order, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static let kept: Set<String> = Set(
        Cases.arms.flatMap { [$0.key + "_order", $0.key + "_blocked"] } + [
            "tess_six_ordered", "tess_ms4_u2_samples_distinct", "readback_paths", "failed_cbs",
        ]
    )

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let scene = try await Cases.Scene(gpu)
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for arm in Cases.arms {
            do {
                Self.judge(arm, try await scene.observe(arm), &metrics, &checks)
            } catch {
                checks.append(("\(arm.key): \(GPU.describe(error))", false))
            }
        }
        metrics.set("readback_paths", gpu.paths.isEmpty ? "-" : String(gpu.paths.text.prefix(80)))
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: Self.kept)
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func judge(
        _ arm: Cases.Arm, _ seen: Cases.Observation, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]
    ) {
        let key = arm.key
        metrics.set("\(key)_error", seen.error?.code ?? 0)
        if let error = seen.error {
            let refused =
                error.domain == MTLCommandBufferErrorDomain
                    && error.code == Int(MTLCommandBufferError.internal.rawValue)
            if refused { metrics.set("\(key)_blocked", "fetch_patches") }
            metrics.set("\(key)_path", String(seen.path.prefix(60)))
            checks.append(
                (
                    "\(key): \(refused ? "no storage form for its pass (gm20b:draw.fetch_patches)" : GPU.describe(error))",
                    false
                )
            )
            return
        }
        checks.append(("\(key): drawn on raw (\(seen.path))", seen.path == "gm20b"))
        let tallies = Cases.tally(arm, seen.images[0])
        var total = tallies.reduce(Cases.Tally(), +)
        total.torn = seen.images.map { Cases.tally(arm, $0).reduce(Cases.Tally(), +).torn }.max() ?? 0
        let stable = seen.images.allSatisfy { $0 == seen.images[0] }
        let apple = zip(tallies, arm.apple).allSatisfy {
            $0.firstThenSecond == $1.first && $0.secondThenFirst == $1.second
        }
        let order = !stable || total.torn > 0 ? "torn" : apple ? "apple" : "stable"
        if arm.fragments > 2 {
            metrics.set("\(key)_ordered", total.firstThenSecond)
        } else {
            metrics.set("\(key)_first_then_second", total.firstThenSecond)
            metrics.set("\(key)_second_then_first", total.secondThenFirst)
        }
        metrics.set("\(key)_torn", total.torn)
        metrics.set("\(key)_other", total.other)
        metrics.set("\(key)_stable", stable)
        metrics.set("\(key)_order", order)
        checks.append(("\(key): never torn over \(seen.images.count) runs", order != "torn"))
        checks.append(("\(key): every pixel in one of the orders", total.other == 0))
        if arm.flat { checks.append(("\(key): patches, then instances, in Metal's order", order == "apple")) }
        if arm.samples > 1 {
            let distinct = Cases.samplesDistinct(seen.images[0])
            metrics.set("\(key)_samples_distinct", distinct)
            checks.append(("\(key): every sample shaded on its own", distinct == Cases.interiorPixels))
        }
    }
}
