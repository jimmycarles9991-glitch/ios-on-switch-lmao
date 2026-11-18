import Foundation

/// gpu.path.import_beside_submits' verdict. A submission that waits behind an import's wiring arrives at a random point of
/// it, so blocked submissions add up to a whole wiring and their p99 nearly all of it, while free ones add only the run's
/// contention. Half a wiring splits the two, and only a wiring long enough that half of it clears both the run's floor and
/// its quiet submissions' own spread can tell them apart (boot 22: 0.28 ms of wiring, 0.26 ms added, undecidable).
public enum ImportBesideVerdict {
    public static let shortestHalfWireMs = 0.5

    public static func addedP99(quiet: [Double], beside: [Double]) -> Double {
        Stats.percentile(beside, 0.99) - Stats.percentile(quiet, 0.99)
    }

    /// Why the run fails: a wiring too short to split blocked from free submissions, too few submissions beside the
    /// imports, or imports that added half a wiring or more to the submissions' p99
    public static func failures(wireMs: Double, quiet: [Double], beside: [Double], minimumBeside: Int) -> [String] {
        var failures: [String] = []
        let half = wireMs / 2
        let spread = Stats.percentile(quiet, 0.99) - Stats.percentile(quiet, 0.5)
        if half < max(shortestHalfWireMs, 2 * spread) {
            failures.append(
                String(
                    format: "an import's wiring took %.2f ms: half of it is under %.2f ms, too short to tell", wireMs,
                    max(shortestHalfWireMs, 2 * spread)))
        }
        if beside.count < minimumBeside {
            failures.append("\(beside.count) submissions beside the imports (at least \(minimumBeside))")
        }
        let added = addedP99(quiet: quiet, beside: beside)
        if added >= half {
            failures.append(
                String(
                    format: "imports added %.2f ms to the p99 submission, half or more of an import's %.2f ms wiring",
                    added, wireMs))
        }
        return failures
    }
}
