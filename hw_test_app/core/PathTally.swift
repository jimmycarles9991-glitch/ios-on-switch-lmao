import Foundation

/// Which GPU path drew a test's work, for its log line: each of its own command buffers' path as the raw facade
/// reports it ("raw", "m2v:<reason>:<predicate>", "split:<raw>+<m2v>:<reason>:<predicate>"), and the process's
/// counters before and after (they also count command buffers a framework made, such as Core Image's).
public struct PathTally: Equatable, Sendable {
    public private(set) var paths: [String: Int] = [:]

    public init() {}

    public mutating func add(_ path: String) { paths[path, default: 0] += 1 }

    public mutating func merge(_ other: PathTally) {
        for (path, count) in other.paths { paths[path, default: 0] += count }
    }

    public var isEmpty: Bool { paths.isEmpty }

    /// "raw/4,m2v:state:state.notpack/3": most frequent first.
    public var text: String {
        paths.sorted { $0.value != $1.value ? $0.value > $1.value : $0.key < $1.key }
            .map { "\($0.key)/\($0.value)" }.joined(separator: ",")
    }

    /// Whether there were command buffers and every one went to m2v whole.
    public var onlyM2V: Bool { !paths.isEmpty && paths.keys.allSatisfy { $0.hasPrefix("m2v:") } }

    /// Counter deltas (the facade's switchRawCounters) as log fields: raw_cbs, m2v_cbs, split_cbs and the predicates
    /// that sent work to m2v, most hit first.
    public static func counterMetrics(before: [String: Int], after: [String: Int]) -> Metrics {
        let delta = after.reduce(into: [String: Int]()) { result, entry in
            let change = entry.value - (before[entry.key] ?? 0)
            if change != 0 { result[entry.key] = change }
        }
        var metrics = Metrics()
        metrics.set("raw_cbs", delta["raw"] ?? 0)
        metrics.set("m2v_cbs", delta["m2v"] ?? 0)
        metrics.set("split_cbs", delta["split"] ?? 0)
        let predicates = delta.filter { $0.key.hasPrefix("pred.") }
            .sorted { $0.value != $1.value ? $0.value > $1.value : $0.key < $1.key }
            .map { "\($0.key.dropFirst("pred.".count))/\($0.value)" }
        if !predicates.isEmpty { metrics.set("m2v_why", predicates.joined(separator: ",")) }
        return metrics
    }
}
