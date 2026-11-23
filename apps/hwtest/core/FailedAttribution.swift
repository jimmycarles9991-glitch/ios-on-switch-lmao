import Foundation

/// Whose command buffers gave the failures between two counter reads (FailedCounts). The test's own are read off the
/// error:<predicate> paths of every command buffer it committed; the rest of each predicate's count are command buffers
/// it did not commit (a framework's, another queue's or thread's: foreign). A failure the test expects is its own. Past
/// those, an own failure says the test, or the path it judges, is wrong (own_failed=, own_why=); a foreign one says
/// something it did not commit failed while it ran (foreign_failed=, foreign_why=). Either is red, and the message says
/// which.
public struct FailedAttribution: Sendable {
    public let counts: FailedCounts
    public let expected: [String: Int]
    /// The test's own command buffers, and the ones that failed by predicate.
    public let committed: Int
    public let ownFailed: [String: Int]
    /// Own failures past the expected ones.
    public let unexpectedOwn: [String: Int]
    /// Failures past the own and the expected ones.
    public let foreign: [String: Int]
    /// Expected failures that did not happen.
    public let missing: [String: Int]
    /// Own error paths the counters did not count.
    public let uncounted: [String: Int]
    /// Own command buffers neither on the driver nor failed.
    public let stray: PathTally

    init(_ counts: FailedCounts, expecting expected: [String: Int], own: PathTally) {
        let want = expected.filter { $0.value != 0 }
        self.counts = counts
        self.expected = want
        committed = own.paths.values.reduce(0, +)
        var failed: [String: Int] = [:]
        var stray = PathTally()
        for (path, count) in own.offRaw.paths {
            if path.hasPrefix("error:") {
                failed[String(path.dropFirst("error:".count)), default: 0] += count
            } else {
                stray.add(path, count)
            }
        }
        ownFailed = failed
        self.stray = stray
        let names = Set(counts.predicates.keys).union(failed.keys).union(want.keys)
        let d = { counts.predicates[$0] ?? 0 }
        let o = { failed[$0] ?? 0 }
        let e = { want[$0] ?? 0 }
        let positive = { (count: (String) -> Int) in
            names.reduce(into: [String: Int]()) { if count($1) > 0 { $0[$1] = count($1) } }
        }
        unexpectedOwn = positive { o($0) - e($0) }
        foreign = positive { d($0) - max(o($0), e($0)) }
        missing = positive { e($0) - d($0) }
        uncounted = positive { o($0) - d($0) }
    }

    var summed: Bool { counts.total == counts.predicates.values.reduce(0, +) }

    public var holds: Bool {
        counts.answered && summed && unexpectedOwn.isEmpty && foreign.isEmpty && missing.isEmpty && uncounted.isEmpty
            && stray.isEmpty
    }

    /// own_failed= and own_why=, foreign_failed= and foreign_why=: each where there is one.
    public var metrics: Metrics { Self.causes(unexpectedOwn, foreign) }

    /// The cause fields of a test that judged several attributions (an arm each).
    public static func metrics(_ attributions: [FailedAttribution]) -> Metrics {
        var own: [String: Int] = [:]
        var foreign: [String: Int] = [:]
        for attribution in attributions {
            own.merge(attribution.unexpectedOwn, uniquingKeysWith: +)
            foreign.merge(attribution.foreign, uniquingKeysWith: +)
        }
        return causes(own, foreign)
    }

    static func causes(_ own: [String: Int], _ foreign: [String: Int]) -> Metrics {
        var metrics = Metrics()
        if !own.isEmpty {
            metrics.set("own_failed", own.values.reduce(0, +))
            metrics.set("own_why", FailedCounts.text(own))
        }
        if !foreign.isEmpty {
            metrics.set("foreign_failed", foreign.values.reduce(0, +))
            metrics.set("foreign_why", FailedCounts.text(foreign))
        }
        return metrics
    }

    /// The label of what holds, or each failure named by whose it was: "own failure: ..." (the test's own command
    /// buffers) and "foreign failure: ..." (ones it did not commit).
    public var check: (String, Bool) {
        let label = expected.isEmpty ? "no command buffer failed" : "exactly \(FailedCounts.text(expected)) failed"
        guard counts.answered else { return (label + ": the facade's counters answer no failed count", false) }
        var parts: [String] = []
        if !unexpectedOwn.isEmpty {
            let past = expected.isEmpty ? "" : " past the expected \(FailedCounts.text(expected))"
            parts.append("own failure: its own command buffers failed \(FailedCounts.text(unexpectedOwn))\(past)")
        }
        if !missing.isEmpty {
            parts.append("own failure: expected \(FailedCounts.text(missing)) more to fail (failed \(counts.text))")
        }
        if !stray.isEmpty { parts.append("own failure: its own command buffers off the driver \(stray.text)") }
        if !uncounted.isEmpty {
            parts.append("own failure: its own paths failed \(FailedCounts.text(uncounted)) the counters did not count")
        }
        if !summed { parts.append("failed_cbs=\(counts.total) is not the sum of \(counts.text)") }
        if !foreign.isEmpty {
            var part =
                "foreign failure: foreign_failed=\(foreign.values.reduce(0, +)) \(FailedCounts.text(foreign)) in "
                    + "command buffers it did not commit (a framework's, another queue's or thread's)"
            if parts.isEmpty {
                part += committed == 0 ? "; it committed none itself" : "; its own \(committed) ended as expected"
            }
            parts.append(part)
        }
        return parts.isEmpty ? (label, true) : (parts.joined(separator: "; "), false)
    }
}
