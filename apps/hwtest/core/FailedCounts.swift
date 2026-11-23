import Foundation

/// The facade's failed command buffers and skipped calls between two reads of its counters (switchMetalCounters,
/// debug builds). `failed` counts every command buffer that completed with an error, whoever made it (RenderBox, Core
/// Image, MPS, Core Animation or the test itself), and `failed.<predicate>` / `failed.none` name what gave each its
/// error, the predicate its path names too (error:<predicate>, error:none). `skipped` counts the calls Metal's
/// validation stops that the facade ran as Apple's driver runs them with validation off (nothing, or the part inside: a
/// clamp or a cut), `skipped.<predicate>` by predicate. Fields: failed_cbs= and failed_why= ("call.sampler/1,none/2",
/// most first); skipped_calls= and skipped_why= where a call was skipped.
public struct FailedCounts: Equatable, Sendable {
    public static let failedKey = "failed"
    public static let skippedKey = "skipped"
    /// The fields a line keeps when it is cut to fit os_log (Metrics.fitLog): a red's cause, which Run All's roll-up
    /// and tools/devicerun read.
    public static let causeKeys: Set<String> = ["own_failed", "own_why", "foreign_failed", "foreign_why", "skipped_why"]

    /// Whether the later read answered `failed` at all: a facade whose counters lack it judges nothing.
    public let answered: Bool
    public let total: Int
    public let predicates: [String: Int]
    public let skipsAnswered: Bool
    public let skipped: Int
    public let skippedPredicates: [String: Int]

    public init(before: [String: Int], after: [String: Int]) {
        answered = after[Self.failedKey] != nil
        total = (after[Self.failedKey] ?? 0) - (before[Self.failedKey] ?? 0)
        predicates = Self.changes(under: Self.failedKey, before, after)
        skipsAnswered = after[Self.skippedKey] != nil
        skipped = (after[Self.skippedKey] ?? 0) - (before[Self.skippedKey] ?? 0)
        skippedPredicates = Self.changes(under: Self.skippedKey, before, after)
    }

    public var text: String { Self.text(predicates) }

    public var metrics: Metrics {
        var metrics = Metrics()
        metrics.set("failed_cbs", answered ? .integer(total) : .text("unanswered"))
        if !predicates.isEmpty { metrics.set("failed_why", text) }
        if skipped != 0 || !skippedPredicates.isEmpty {
            metrics.set("skipped_calls", skipped)
            metrics.set("skipped_why", Self.text(skippedPredicates))
        }
        return metrics
    }

    /// Whose command buffers the failures were, given the ones the test expects of its own and the paths of every
    /// command buffer it committed (`own`: GPU.paths, or the buffers it made itself).
    public func attributed(expecting expected: [String: Int] = [:], own: PathTally) -> FailedAttribution {
        FailedAttribution(self, expecting: expected, own: own)
    }

    /// FailedAttribution.check.
    public func check(expecting expected: [String: Int] = [:], own: PathTally) -> (String, Bool) {
        attributed(expecting: expected, own: own).check
    }

    /// The check that exactly `expected` calls were skipped between the reads, by predicate, and no other: for work a
    /// framework records, none. It fails where the counters answer no `skipped`.
    public func skipCheck(expecting expected: [String: Int] = [:]) -> (String, Bool) {
        let want = expected.filter { $0.value != 0 }
        let label = want.isEmpty ? "no call was skipped" : "exactly \(Self.text(want)) skipped"
        guard skipsAnswered else { return (label + ": the facade's counters answer no skipped count", false) }
        let holds = skipped == want.values.reduce(0, +) && skippedPredicates == want
        return (holds ? label : "\(label), not \(Self.text(skippedPredicates)) (skipped_calls=\(skipped))", holds)
    }

    /// The failure fields and their cause on `metrics`, and FailedAttribution.check, over the counters `before` and
    /// `after`.
    public static func judged(
        before: [String: Int], after: [String: Int], expecting expected: [String: Int] = [:], own: PathTally,
        _ metrics: inout Metrics
    ) -> (String, Bool) {
        let counts = FailedCounts(before: before, after: after)
        let attribution = counts.attributed(expecting: expected, own: own)
        metrics.merge(counts.metrics)
        metrics.merge(attribution.metrics)
        return attribution.check
    }

    /// `judged` for work a framework records (Core Animation, RenderBox, Core Image, MPS), and its skips: none.
    public static func judgedFramework(
        before: [String: Int], after: [String: Int], own: PathTally, _ metrics: inout Metrics
    ) -> [(String, Bool)] {
        let failed = judged(before: before, after: after, own: own, &metrics)
        return [failed, FailedCounts(before: before, after: after).skipCheck()]
    }

    static func changes(under key: String, _ before: [String: Int], _ after: [String: Int]) -> [String: Int] {
        let prefix = key + "."
        return after.reduce(into: [:]) { out, entry in
            guard entry.key.hasPrefix(prefix) else { return }
            let change = entry.value - (before[entry.key] ?? 0)
            if change != 0 { out[String(entry.key.dropFirst(prefix.count))] = change }
        }
    }

    /// "call.sampler/1,none/2": most first, "-" for none.
    public static func text(_ counts: [String: Int]) -> String {
        guard !counts.isEmpty else { return "-" }
        return counts.sorted { $0.value != $1.value ? $0.value > $1.value : $0.key < $1.key }
            .map { "\($0.key)/\($0.value)" }.joined(separator: ",")
    }

    /// The counts of a `text` field, nil for one that is not.
    public static func counts(_ text: String) -> [String: Int]? {
        guard text != "-" else { return [:] }
        var counts: [String: Int] = [:]
        for item in text.split(separator: ",") {
            guard let slash = item.lastIndex(of: "/"), let count = Int(item[item.index(after: slash)...]) else {
                return nil
            }
            counts[String(item[..<slash]), default: 0] += count
        }
        return counts
    }
}
