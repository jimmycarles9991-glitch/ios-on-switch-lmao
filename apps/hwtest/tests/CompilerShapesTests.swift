import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import HWTestKit
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

/// Half powr at every special operand: powr_specials over each pair of CompilerShapes.powrHalves (the specials, class
/// boundaries and two curve exponents, 400 pairs), each answer against Metal's (CompilerShapes.powrRight: its special
/// answers exactly, the rest within two half ulps). The shader compiler gives a half powr on GM20B as exp2(y * log2(x))
/// alone, with none of the selects around it that answer the specials on other devices: this is that rule's proof,
/// judged on the special lanes (wrong_special); `wrong` adds the finite ones, whose precision is MUFU's.
struct PowrSpecialsTest: HWTest {
    let id = "gpu.gm20b.powr_specials"
    let name = "Half powr at every special operand, as Metal answers, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let (answers, timing) = try await CompilerShapes.powrAnswers(gpu)
            guard let answers else { return [("powr_specials failed: \(timing.facadePath)", false)] }
            let pairs = CompilerShapes.powrPairs
            let operands = pairs.map { (Float16(bitPattern: $0.x), Float16(bitPattern: $0.y)) }
            let wrong = pairs.indices.filter { !CompilerShapes.powrRight(operands[$0].0, operands[$0].1, answers[$0]) }
            let special = pairs.indices.filter { CompilerShapes.powrSpecial(operands[$0].0, operands[$0].1) != nil }
            let wrongSpecial = wrong.filter(special.contains)
            metrics.set("wrong", wrong.count)
            metrics.set("wrong_special", wrongSpecial.count)
            metrics.set("pairs", pairs.count)
            if let i = wrongSpecial.first ?? wrong.first {
                let hex = { (v: UInt16) in String(v, radix: 16) }
                metrics.set("first_wrong", "\(hex(pairs[i].x))_\(hex(pairs[i].y)):\(answers[i])")
            }
            return [("wrong_special=\(wrongSpecial.count) of \(special.count)", wrongSpecial.isEmpty)]
        }
    }
}

/// Half fmax, fmin and a clamp over every pair of CompilerShapes.powrHalves (400: the specials, both zeros, NaNs of
/// both signs, class boundaries), against Apple's GPUs' answers (CompilerShapes.metalMax, metalMin: x where x > y, or
/// x < y, or where y is a NaN; y otherwise, so a tie of +0 and -0 gives y), and a transfer curve's tests of a half
/// (abs(x) < y, x < 0). The shader compiler gives a half's min and max on GM20B as HSETP2 and SEL, and those tests as
/// HSETP2 with |x| as an operand modifier and zero as RZ, with no F2F to f32 and back.
struct HalfMinMaxTest: HWTest {
    let id = "gpu.gm20b.half_min_max"
    let name = "Half fmax, fmin and clamp over every special pair, as Metal defines them, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let (answers, timing) = try await CompilerShapes.minMaxAnswers(gpu)
            guard let answers else { return [("half_min_max failed: \(timing.facadePath)", false)] }
            let pairs = CompilerShapes.powrPairs
            let wrong = pairs.indices.filter { i in
                let (x, y) = (Float16(bitPattern: pairs[i].x), Float16(bitPattern: pairs[i].y))
                let hi = CompilerShapes.metalMax(x, y)
                return !CompilerShapes.sameHalf(answers[i].x, hi)
                    || !CompilerShapes.sameHalf(answers[i].y, CompilerShapes.metalMin(x, y))
                    || !CompilerShapes.sameHalf(answers[i].z, CompilerShapes.metalMin(hi, 1))
                    || answers[i].w != CompilerShapes.curveTests(x, y)
            }
            metrics.set("wrong", wrong.count)
            metrics.set("pairs", pairs.count)
            if let i = wrong.first {
                let hex = { (v: UInt16) in String(v, radix: 16) }
                metrics.set("first_wrong", "\(hex(pairs[i].x))_\(hex(pairs[i].y)):\(answers[i])")
            }
            return [("wrong=\(wrong.count) of \(pairs.count)", wrong.isEmpty)]
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
