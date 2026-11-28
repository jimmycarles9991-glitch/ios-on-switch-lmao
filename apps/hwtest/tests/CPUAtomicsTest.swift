import Foundation
import HWTestCore
import HWTestKit
import SwitchAtomics

/// Every LSE form an A12 build emits (atomics/switch_atomics_list.h: CAS at each size and CASP, the eight LD<op>s and
/// SWP at each size and ordering, the register aliasings, x30 as an operand, a site with every register live), each
/// from more threads than cores with invariants a lost or doubled update breaks (atomics/switch_atomics_hammer.c), and
/// the same operations through Apple's entry points in the dyld cache. The 27 app's forms are its own code, which the
/// a64x loader translates at load (a B per site to a stub after the image): neither the cache plan nor the on-disk
/// pretranslate. Apple's entry points run the cache plan's stubs on 27 and Apple's ARMv8.0 code on 17, where the
/// app's forms are the ARMv8.0 sequences those stubs run. The store-buffering rows say whether this CPU lets a load
/// pass an LDAXR/STLXR pair and whether an AL atomic, as translated, forbids it as the instruction does.
struct CPUAtomicsTest: HWTest {
    let id = "cpu.atomics"
    let name = "Every LSE form, Apple's atomic entry points and store buffering under contention"
    let subsystem = Subsystem.cpu
    var timeout: TimeInterval { 300 }

    static let (threads, ops, litmusRuns) = (8, 2_000, 2_000_000)

    func run(_ context: TestContext) async throws -> Outcome {
        let before = A64XSelfTest.sysctls().values
        let lse = switch_atomics_lse()
        var metrics = Metrics()
        metrics.set("lse", lse)
        let forms = try Self.rows(switch_atomics_forms(), switch_atomics_form)
        let library = try Self.rows(switch_atomics_library_forms(), switch_atomics_library_form)
        var litmus: [switch_litmus_row] = []
        for index in 0 ..< switch_atomics_litmus_tests() {
            var row = switch_litmus_row()
            guard switch_atomics_litmus(index, UInt64(Self.litmusRuns), &row) else {
                throw TestFailure("litmus \(index): its two threads could not be made")
            }
            litmus.append(row)
        }
        let (after, _) = A64XSelfTest.sysctls()
        let delta = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }

        let broken = (forms + library).filter { $0.violations + $0.clobbers > 0 }
        let unbranched = forms.filter { $0.branched != lse }
        let litmusUnbranched = litmus.filter { $0.branched != ($0.has_site && lse) }
        let reordered = litmus.filter { $0.judged && $0.forbidden > 0 }
        metrics.set("forms", forms.count)
        metrics.set("library_forms", library.count)
        metrics.set("violations", Int((forms + library).reduce(0) { $0 + $1.violations }))
        metrics.set("clobbers", Int((forms + library).reduce(0) { $0 + $1.clobbers }))
        metrics.set("site_mismatches", unbranched.count + litmusUnbranched.count)
        if !broken.isEmpty {
            let named = broken.map { "\(Self.text($0.name)):\($0.violations)/\($0.clobbers)" }
            metrics.set("broken", named.joined(separator: ","))
        }
        if !unbranched.isEmpty || !litmusUnbranched.isEmpty {
            let names = unbranched.map { Self.text($0.name) } + litmusUnbranched.map { Self.text($0.name) }
            metrics.set("site_mismatch", names.joined(separator: ","))
        }
        for name in ["traps", "traps_unhandled", "stub_mismatch"] where !after.isEmpty {
            metrics.set("d_" + name, delta(name))
        }
        for row in litmus {
            let name = Self.text(row.name)
            metrics.set(name + "_forbidden", Int(row.forbidden))
            metrics.set(name + "_runs", Int(row.runs))
            metrics.set(name + "_runs_per_s", Double(row.runs) * 1e9 / Double(max(row.ns, 1)), digits: 0)
        }
        for row in forms + library {
            metrics.set(Self.text(row.name) + "_kops", Double(row.ops) * 1e6 / Double(max(row.ns, 1)), digits: 0)
        }

        var checks: [(String, Bool)] = [
            ("every form held its invariants and left NZCV and live registers alone", broken.isEmpty),
            ("this build's forms are \(lse ? "LSE" : "ARMv8.0") as its iOS needs", lse == (context.ios >= 26)),
            (
                lse ? "every form's site maps as a branch to a stub" : "no form's site maps as a branch",
                unbranched.isEmpty && litmusUnbranched.isEmpty
            ),
        ]
        let reorderedNames = reordered.map { Self.text($0.name) }.joined(separator: ",")
        checks.append(("no row that must order saw store buffering \(reorderedNames)", reordered.isEmpty))
        if lse {
            checks += [
                ("no LSE site took the trap path (d_traps 0)", delta("traps") == 0),
                ("no unhandled instruction (d_traps_unhandled 0)", delta("traps_unhandled") == 0),
                ("every page's stubs matched (d_stub_mismatch 0)", delta("stub_mismatch") == 0),
            ]
        }
        return .judged(checks, metrics)
    }

    static func rows(
        _ count: UInt32, _ run: (UInt32, UInt32, UInt32, UnsafeMutablePointer<switch_atomics_row>?) -> Bool
    ) throws -> [switch_atomics_row] {
        try (0 ..< count).map { index in
            var row = switch_atomics_row()
            guard run(index, UInt32(threads), UInt32(ops), &row) else {
                throw TestFailure("form \(index): its threads could not be made")
            }
            return row
        }
    }

    /// A NUL-terminated char array the harness filled.
    static func text<T>(_ field: T) -> String {
        withUnsafeBytes(of: field) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
    }
}
