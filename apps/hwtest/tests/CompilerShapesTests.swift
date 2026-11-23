import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The shader compiler's branch layout: sm53_layout_paths (nested if/else, a loop with continue and break, every
/// pixel's branches its own, so branch targets fall in each slot of an instruction group) over a 256x256 RGBA32F
/// target, against the same code on the CPU (CompilerShapes.layout), which an M4 Max matches at every pixel
/// (wrong_pixels 0; 43 us a draw). us: this draw's GPU time.
struct SM53LayoutTest: HWTest {
    let id = "gpu.gm20b.sm53_layout"
    let name = "Nested branches and a loop with continue and break, the path every pixel took, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let r = try await CompilerShapes.observeLayout(gpu)
            metrics.set("wrong_pixels", r.wrong)
            metrics.set("first_wrong", r.first)
            metrics.set("us", r.micros, digits: 1)
            return [("wrong_pixels=\(r.wrong) (\(r.path))", r.wrong == 0)]
        }
    }
}

/// Floating-point classes and comparisons: float_classes over every special value and class boundary of both signs
/// (CompilerShapes.floatValues, 40), each value's isnan, isinf, isfinite, isnormal, signbit and comparisons of x and |x|
/// with 0, FLT_MIN, 1, FLT_MAX and inf as bits, against what an M4 Max writes with Metal's default fast math (wrong 0
/// of 40 against CompilerShapes.classes; 20 of the 40 differ from IEEE 754: the 10 denormals classify as zeros, the 10
/// NaNs answer |x| > 0 and not x != x).
struct FloatTestsTest: HWTest {
    let id = "gpu.gm20b.float_tests"
    let name = "isnan, isinf, isfinite, isnormal and comparisons over every special float, as Metal answers, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let (bits, timing) = try await CompilerShapes.floatBits(gpu)
            guard let bits else { return [("float_classes failed: \(timing.facadePath)", false)] }
            let wrong = CompilerShapes.floatValues.indices
                .filter { bits[$0] != CompilerShapes.classes(CompilerShapes.floatValues[$0]) }
            metrics.set("wrong", wrong.count)
            metrics.set("values", bits.count)
            if let i = wrong.first {
                let v = CompilerShapes.floatValues[i]
                metrics.set(
                    "first_wrong",
                    "0x\(String(v.bitPattern, radix: 16)):\(String(bits[i], radix: 2))_not_\(String(CompilerShapes.classes(v), radix: 2))"
                )
            }
            return [("wrong=\(wrong.count) of \(bits.count)", wrong.isEmpty)]
        }
    }
}

enum CompilerShapeRun {
    /// `body`'s checks on the facade's device, with its failures, skips and paths judged
    static func judged(
        _ body: (GPU, inout Metrics) async throws -> [(String, Bool)]
    ) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)]
        do {
            checks = try await body(gpu, &metrics)
        } catch {
            checks = [("\(GPU.describe(error))", false)]
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
