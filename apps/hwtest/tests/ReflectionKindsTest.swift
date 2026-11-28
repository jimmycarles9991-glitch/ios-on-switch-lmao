import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUModels
import HWTestKit
import Metal

/// Pipeline reflection per binding kind (drivers/TegraGM20BMetal/TegraGM20BMetalReflectionObjects.mm), two arms
/// judged apart. kinds: reflection_kinds's compute state and reflection_vs/reflection_fs's render state (BGRA8Unorm),
/// made with binding and buffer type info, and on the versions whose QuartzCore has them its compute_average_luma and
/// compute_sum_luma (qc_kernels): every binding answers type, access, index, arrayLength, isUsed, isActive, isArgument
/// and its own kind's selectors as an M4 Max's Metal does (ReflectionKindsGoldens; mismatches, of them
/// array_length_mismatches, first_mismatch), responds to MTLBindingInternal's 21 selectors and not to dataType, offset,
/// elementType or members (responds_mismatches); a state not made is named apart (unmade); the pointer and struct types
/// every binding has a method for are reported (kinds_reported_off). wrong_kind (ReflectionWrongKind): the first
/// binding of each type of each state asked every other type's selector reports one failure through Metal's
/// MTLReportFailure, type 0, Metal's message, from a function ending " <selector>]", then answers Metal's fixed value
/// (wrong_kind_asks: 76 on 21H461, 116 on 24B119; wrong_kind_reported equal to it; wrong_kind_off 0,
/// wrong_kind_first_off); its own type's selectors report none (own_kind_reports 0). Each ask runs with the test's
/// report block set: with none Metal's default aborts the process. failure_mode and failure_enabled are the device
/// Metal's error mode and enable of failure type 0, reported (an M4 Max: 6, the abort, and 1).
struct ReflectionKindsTest: HWTest {
    let id = "gpu.gm20b.reflection_kinds"
    let name = "Every binding kind's reflection answers as Metal's"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    /// The QuartzCore compute kernels each iOS version's library has and this test reflects
    static let systemKernels: [Int: [String]] = [27: ["compute_average_luma", "compute_sum_luma"]]
    /// The wrong-type asks of each iOS version's states (buffer 7, threadgroup memory 9, texture 8, sampler 11 each)
    static let wrongKindAsks: [Int: Int] = [17: 76, 27: 116]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let system = Self.systemKernels[context.ios] ?? []
        let made = ReflectionKinds.made(gpu.device, gpu.library, system: system) {
            try gpu.device.makeLibrary(URL: CAState.library)
        }
        let compared = ReflectionKinds.compare(made.seen, ReflectionKindsGoldens.metal, functions: made.functions)
        var metrics = Metrics()
        metrics.set("unmade", made.unmade.isEmpty ? "-" : made.unmade.joined(separator: ","))
        metrics.set(
            "qc_kernels",
            system.isEmpty ? "absent" : system.allSatisfy(made.functions.contains) ? "compared" : "unmade"
        )
        metrics.set("bindings", compared.bindings)
        metrics.set("mismatches", compared.mismatches)
        metrics.set("array_length_mismatches", compared.arrayLengthMismatches)
        metrics.set("responds_mismatches", compared.respondsMismatches)
        metrics.set("first_mismatch", compared.firstMismatch)
        metrics.set("kinds_reported_off", compared.reportedOff)
        var checks: [(String, Bool)] = [
            ("kinds: a state not made (\(made.unmade.joined(separator: ",")))", made.unmade.isEmpty),
            (
                "kinds: \(compared.mismatches) answers off Metal's over \(compared.bindings) bindings, first "
                    + compared.firstMismatch, compared.mismatches == 0 && compared.bindings > 0
            ),
            ("kinds: \(compared.respondsMismatches) respondsToSelector answers off", compared.respondsMismatches == 0),
        ]
        guard let functions = ReflectionWrongKind.Functions.load() else {
            checks.append(("wrong_kind: Metal exports no report-failure block: no wrong-type selector asked", false))
            return .judged(checks, metrics, proof: gpu.proof)
        }
        let counted = ReflectionWrongKind.asked(made.seen, functions)
        let want = Self.wrongKindAsks[context.ios] ?? -1
        metrics.set("wrong_kind_asks", counted.asks)
        metrics.set("wrong_kind_reported", counted.reported)
        metrics.set("wrong_kind_off", counted.off)
        metrics.set("wrong_kind_first_off", counted.firstOff)
        metrics.set("own_kind_reports", counted.ownReports)
        metrics.set("failure_mode", Int(functions.mode(0)))
        metrics.set("failure_enabled", functions.enabled(0))
        checks += [
            ("wrong_kind: \(counted.asks) asks, want \(want)", counted.asks == want),
            ("wrong_kind: \(counted.reported) reports over \(counted.asks) asks", counted.reported == counted.asks),
            ("wrong_kind: \(counted.off) asks off Metal's report, first \(counted.firstOff)", counted.off == 0),
            ("wrong_kind: \(counted.ownReports) reports for a binding's own selectors", counted.ownReports == 0),
        ]
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
