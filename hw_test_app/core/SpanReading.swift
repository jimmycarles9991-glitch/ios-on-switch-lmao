import Foundation

/// The debug kernel's spans line (drivers/TegraGM20B/SwitchGpuSpanNotes.h):
/// "job_max_us=N <totals> spans=K; <owner>: <span>; ...". K counts the notes that were waiting; the line holds as many
/// as fit and the rest come with the next ask.
/// The totals are the compositor's rounds (SwitchGpuCompositorRound.h) and its sync pushes (SwitchGpuCompositorSync.h).
public struct SpanReading: Equatable, Sendable {
    public let jobMaxUs: Int
    public let counts: [String: Int]
    public let spans: [String]

    public init(jobMaxUs: Int, counts: [String: Int], spans: [String]) {
        self.jobMaxUs = jobMaxUs
        self.counts = counts
        self.spans = spans
    }

    /// Whether the line held every note that was waiting
    public var complete: Bool { spans.count >= (counts["spans"] ?? 0) }

    public static func parse(_ line: String) -> SpanReading {
        let parts = line.components(separatedBy: "; ")
        var counts: [String: Int] = [:]
        for field in (parts.first ?? "").split(separator: " ") {
            let pair = field.split(separator: "=", maxSplits: 1)
            if pair.count == 2, let value = Int(pair[1]) { counts[String(pair[0])] = value }
        }
        return SpanReading(jobMaxUs: counts["job_max_us"] ?? 0, counts: counts, spans: Array(parts.dropFirst()))
    }

    /// Lines asked one after another, as one reading over their whole time: the longest job and the longest sync, every
    /// other total added, the spans in order and counted
    public static func merged(_ readings: [SpanReading]) -> SpanReading {
        var counts: [String: Int] = [:]
        for reading in readings {
            for (key, value) in reading.counts {
                counts[key] = key.hasSuffix("_max_us") ? max(counts[key] ?? 0, value) : (counts[key] ?? 0) + value
            }
        }
        let spans = readings.flatMap(\.spans)
        counts["spans"] = spans.count
        return SpanReading(jobMaxUs: counts["job_max_us"] ?? 0, counts: counts, spans: spans)
    }
}

/// One of the spans line's notes, "<owner>: <us> us k<key> <state> ..." (raw_span_marks.h formatSpan). The key is read
/// as the one field that is "k" and 16 hex digits, so a line from before the key was the third field ("<us> us <state>
/// k<key> ...", boot 22) reads the same, and the state's name, which a span may lack, is never looked at.
public struct LongSpan: Equatable, Sendable {
    public let owner: String
    public let us: Int
    public let key: UInt64

    public init(owner: String, us: Int, key: UInt64) {
        self.owner = owner
        self.us = us
        self.key = key
    }

    public static func parse(_ note: String) -> LongSpan? {
        guard let colon = note.range(of: ": ") else { return nil }
        let fields = note[colon.upperBound...].split(separator: " ")
        guard fields.count >= 3, fields[1] == "us", let us = Int(fields[0]) else { return nil }
        for field in fields.dropFirst(2) where field.count == 17 && field.first == "k" {
            if let key = UInt64(field.dropFirst(), radix: 16) {
                return LongSpan(owner: String(note[..<colon.lowerBound]), us: us, key: key)
            }
        }
        return nil
    }
}

/// The generic merge states of the build's QuartzCore by key (hw_test_app/ca_generic_keys.py: "<key> <name>" lines)
public struct GenericStates: Sendable {
    public let names: [UInt64: String]

    public init(names: [UInt64: String]) { self.names = names }

    public static func parse(_ text: String) -> GenericStates {
        var names: [UInt64: String] = [:]
        for line in text.split(separator: "\n") {
            let fields = line.split(separator: " ", maxSplits: 1)
            if fields.count == 2, let key = UInt64(fields[0], radix: 16) { names[key] = String(fields[1]) }
        }
        return GenericStates(names: names)
    }

    /// The reading's spans of a generic state, in order
    public func spans(in reading: SpanReading) -> [LongSpan] {
        reading.spans.compactMap(LongSpan.parse).filter { names[$0.key] != nil }
    }
}
