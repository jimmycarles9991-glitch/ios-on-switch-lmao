import Foundation

/// Run All's account of what reddened its tests and what they skipped, from each test's fields in run order, logged
/// once ahead of the summary (LogLine.rollUp). failed_reds=: each predicate a failure past a test's expectation named
/// (own_why=, foreign_why=; FailedAttribution), the number of tests it reddened, the first of them and whether that
/// failure was the test's own or foreign: "enc.pass/12@gpu.gm20b.xr_srgb:own,none/3@gpu.ca.blur_chain:foreign", in the
/// order the first reds came. skipped_tests=: each predicate of a skipped call (skipped_why=), the tests that skipped
/// one and the first: "call.blit/2@gpu.gm20b.invalid_requests".
public struct FailedRollUp: Equatable, Sendable {
    public struct Entry: Equatable, Sendable {
        public let predicate: String
        public let first: String
        /// "own" or "foreign" for a failure, nil for a skip.
        public let whose: String?
        public fileprivate(set) var tests = 1

        public var text: String { "\(predicate)/\(tests)@\(first)" + (whose.map { ":" + $0 } ?? "") }
    }

    /// The longest each field's entries may run before the rest are counted ("+3"): the line stays under os_log's
    /// ~1 KiB.
    static let fieldBudget = 400

    public private(set) var reds: [Entry] = []
    public private(set) var skips: [Entry] = []

    /// `records` in the order they ran.
    public init(_ records: [TestRecord]) {
        for record in records {
            let metrics = record.outcome.metrics
            if record.outcome.verdict == .fail {
                var named: Set<String> = []
                for whose in ["own", "foreign"] {
                    for predicate in Self.predicates(metrics[whose + "_why"]) where named.insert(predicate).inserted {
                        Self.count(&reds, predicate, record.id, whose)
                    }
                }
            }
            for predicate in Self.predicates(metrics["skipped_why"]) { Self.count(&skips, predicate, record.id, nil) }
        }
    }

    public var outcome: Outcome {
        var metrics = Metrics()
        metrics.set("failed_reds", Self.field(reds))
        metrics.set("skipped_tests", Self.field(skips))
        return Outcome(reds.isEmpty ? .pass : .fail, metrics)
    }

    static func count(_ entries: inout [Entry], _ predicate: String, _ id: String, _ whose: String?) {
        if let index = entries.firstIndex(where: { $0.predicate == predicate }) {
            entries[index].tests += 1
        } else {
            entries.append(Entry(predicate: predicate, first: id, whose: whose))
        }
    }

    static func predicates(_ value: MetricValue?) -> [String] {
        guard case .text(let text) = value, text != "-" else { return [] }
        return text.split(separator: ",").compactMap { item in item.lastIndex(of: "/").map { String(item[..<$0]) } }
    }

    static func field(_ entries: [Entry]) -> String {
        var text = ""
        for (index, entry) in entries.enumerated() {
            let item = (text.isEmpty ? "" : ",") + entry.text
            guard text.count + item.count <= fieldBudget else { return text + ",+\(entries.count - index)" }
            text += item
        }
        return text.isEmpty ? "-" : text
    }
}
