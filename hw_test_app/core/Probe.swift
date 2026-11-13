import Foundation

/// What a hardware probe found about the claim it tests. The test's own PASS only says that the probe ran; this goes in
/// its line as claim_verdict=.
public enum ClaimVerdict: String, Sendable {
    case pass = "PASS"
    case fail = "FAIL"
    case inconclusive = "INCONCLUSIVE"
}

public struct ProbeFinding: Sendable {
    public var verdict: ClaimVerdict
    public var metrics: Metrics
    /// Why the verdict is what it is, for the line's detail.
    public var note: String

    public init(_ verdict: ClaimVerdict, _ metrics: Metrics = Metrics(), note: String = "") {
        self.verdict = verdict
        self.metrics = metrics
        self.note = note
    }

    /// PASS when every count is zero, FAIL otherwise; INCONCLUSIVE when the work did not take the path the claim is
    /// about (`path`: the facade's path of each command buffer that carried it, "raw" when it did).
    public static func counted(_ wrong: [String: Int], paths: [String], metrics: Metrics = Metrics()) -> ProbeFinding {
        var metrics = metrics
        for (name, count) in wrong.sorted(by: { $0.key < $1.key }) { metrics.set(name, count) }
        let offPath = paths.filter { $0 != "raw" }
        if !offPath.isEmpty {
            return ProbeFinding(.inconclusive, metrics, note: "not on the raw path: \(offPath.joined(separator: ","))")
        }
        let failed = wrong.filter { $0.value != 0 }.keys.sorted()
        let note = failed.isEmpty ? "" : "wrong: \(failed.joined(separator: ","))"
        return ProbeFinding(failed.isEmpty ? .pass : .fail, metrics, note: note)
    }

    /// The probe's fields in front of its own: probe=<id> claim_verdict=<verdict>; the longest of its own go to the
    /// report alone where the line would pass os_log's ~1 KiB.
    public func outcome(probe: String, base: Metrics, proof: String) -> Outcome {
        var all = Metrics()
        all.set("probe", probe)
        all.set("claim_verdict", verdict.rawValue)
        all.merge(base)
        all.merge(metrics)
        all.fitLog(within: Self.fieldBudget, keeping: ["probe", "claim_verdict", "rule", "dev", "paths"])
        return .pass(all, proof: proof, detail: String(note.prefix(Self.noteBudget)))
    }

    static let fieldBudget = 720
    static let noteBudget = 160
}
