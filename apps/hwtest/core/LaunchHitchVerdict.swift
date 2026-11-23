import Foundation

/// HW Test ui.launch_hitch's judgement of one launch and the return to the test app, read as one SpanReading: it fails
/// on a compositor job over the bound and names the generic merge states' spans (GenericStates) by key, name and time.
/// Only spans of 5 ms or more are noted (span_marks.h kLongSpanNs), so the generic counts and times are a floor.
public struct LaunchHitchVerdict: Sendable {
    public let reading: SpanReading
    public let generic: [LongSpan]
    public let jobBoundUs: Int
    private let states: GenericStates

    /// The generic spans the result line names; the rest are counted
    static let namedSpans = 3

    public init(reading: SpanReading, states: GenericStates, jobBoundUs: Int) {
        self.reading = reading
        self.generic = states.spans(in: reading)
        self.jobBoundUs = jobBoundUs
        self.states = states
    }

    public var firstGenericMs: Double { Double(generic.first?.us ?? 0) / 1000 }
    public var genericTotalMs: Double { Double(generic.reduce(0) { $0 + $1.us }) / 1000 }

    /// "k<key> <name> <us> us" for the first generic spans, then how many more
    public var genericText: String {
        let named = generic.prefix(Self.namedSpans).map {
            "k\(Self.hex($0.key)) \(states.names[$0.key] ?? "?") \($0.us) us"
        }
        let more = generic.count > Self.namedSpans ? ["+\(generic.count - Self.namedSpans) more"] : []
        return (named + more).joined(separator: ", ")
    }

    static func hex(_ key: UInt64) -> String {
        let digits = String(key, radix: 16)
        return String(repeating: "0", count: max(0, 16 - digits.count)) + digits
    }

    /// Nil where every compositor job of the window kept within the bound
    public var failure: String? {
        guard reading.jobMaxUs > jobBoundUs else { return nil }
        let generics = generic.isEmpty ? "" : "; generic states: \(genericText)"
        return "the compositor's longest job took \(reading.jobMaxUs) us\(generics)"
    }

    /// The result line keeps the job, the counts and the generic spans; the totals and the spans' own text go to the
    /// report only, so two arms fit in what os_log keeps of a line
    public var metrics: Metrics {
        var metrics = Metrics()
        metrics.set("job_max_us", reading.jobMaxUs)
        metrics.set("spans", reading.spans.count)
        metrics.set("generic_spans", generic.count)
        metrics.set("first_generic_ms", firstGenericMs)
        metrics.set("generic_total_ms", genericTotalMs)
        if !generic.isEmpty { metrics.set("generic", genericText) }
        for key in ["sync_subs", "sync_us", "behind", "behind_sync_us", "sync_max_us"] {
            metrics.set(key, reading.counts[key] ?? 0)
            metrics.keepInReportOnly(key)
        }
        for (i, span) in reading.spans.prefix(6).enumerated() {
            metrics.set("span\(i)", span)
            metrics.keepInReportOnly("span\(i)")
        }
        return metrics
    }
}

/// One of ui.launch_hitch's arms: the target app opened, then this app opened again, judged as one window
public struct LaunchHitchArm: Sendable {
    public let name: String
    public let verdict: LaunchHitchVerdict
    /// From asking for the target to this app leaving the foreground; nil where it never left
    public let launchMs: Double?
    /// From asking for this app to its being active again; nil where it was not within the test's wait
    public let returnMs: Double?

    public init(name: String, verdict: LaunchHitchVerdict, launchMs: Double?, returnMs: Double?) {
        self.name = name
        self.verdict = verdict
        self.launchMs = launchMs
        self.returnMs = returnMs
    }

    /// The test's outcome over its arms, each field prefixed with its arm's name: it fails on any arm's job over the
    /// bound. `target` is whether the target app ran when the test began; the app has no way to end it, so where it ran
    /// or where that is unknown there is no cold arm, and `cold` says why.
    public static func outcome(_ arms: [LaunchHitchArm], target: LaunchTarget, genericStates: Int) -> Outcome {
        var metrics = Metrics()
        metrics.set("target_running", target.rawValue)
        metrics.set("generic_states", genericStates)
        if !arms.contains(where: { $0.name == "cold" }) { metrics.set("cold", "skipped_target_" + target.rawValue) }
        var failures: [String] = []
        for arm in arms {
            metrics.set(arm.name + "_launch_ms", arm.launchMs ?? -1, digits: 0)
            metrics.set(arm.name + "_return_ms", arm.returnMs ?? -1, digits: 0)
            metrics.merge(arm.verdict.metrics, prefix: arm.name + "_")
            if let failure = arm.verdict.failure { failures.append("\(arm.name): \(failure)") }
        }
        return failures.isEmpty ? .pass(metrics) : .fail(LogLine.fitted(failures), metrics)
    }
}
