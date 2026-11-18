import Foundation

/// The os_log line tools/devicerun/hwtest.py reads, one per test:
///
///     HWTEST <id> <PASS|FAIL|SKIP> <metric>=<value>... [hw=<proof>] ms=<duration> [why=<detail>]
///
/// Fields are separated by single spaces and no value contains one, so `split(" ")` and the first "=" of each field
/// parse it. `why` comes last because it is free text.
public enum LogLine {
    public static let prefix = "HWTEST"
    public static let suiteID = "suite"
    public static let reservedKeys: Set<String> = ["hw", "ms", "why"]

    public static func isValidKey(_ key: String) -> Bool {
        !key.isEmpty
            && key.unicodeScalars.allSatisfy { ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "_" }
    }

    public static func isValidID(_ id: String) -> Bool {
        !id.isEmpty && id.split(separator: ".", omittingEmptySubsequences: false).allSatisfy { isValidKey(String($0)) }
    }

    /// Whitespace becomes "_", so a value never splits the line.
    public static func sanitized(_ text: String) -> String {
        let collapsed = text.split(whereSeparator: { $0.isWhitespace || $0.isNewline }).joined(separator: "_")
        return collapsed.isEmpty ? "-" : collapsed
    }

    /// Reasons joined for why=, which comes last in a message os_log cuts near 1 KiB: the first whole, the rest
    /// while they fit in `budget` characters, then how many were left out.
    public static func fitted(_ parts: [String], budget: Int = 500) -> String {
        var text = parts.first ?? ""
        for (index, part) in parts.enumerated().dropFirst() {
            guard text.count + part.count + 2 <= budget else { return text + "; +\(parts.count - index) more" }
            text += "; " + part
        }
        return text
    }

    public static func format(_ record: TestRecord) -> String {
        format(id: record.id, outcome: record.outcome, milliseconds: record.milliseconds)
    }

    public static func format(id: String, outcome: Outcome, milliseconds: Double) -> String {
        precondition(isValidID(id), "test id \(id) is not dotted [a-z0-9_]+")
        var fields = [prefix, id, outcome.verdict.rawValue]
        fields += outcome.metrics.logged.map { "\($0.0)=\($0.1.logText)" }
        if !outcome.proof.isEmpty { fields.append("hw=" + sanitized(outcome.proof)) }
        fields.append("ms=" + MetricValue.format(milliseconds, digits: 1))
        if !outcome.detail.isEmpty { fields.append("why=" + sanitized(outcome.detail)) }
        return fields.joined(separator: " ")
    }

    /// The run's counts and the iOS version the app is built for: tools/devicerun judges a version's own tests only
    /// in a log whose summary names that version.
    public static func summary(_ records: [TestRecord], ios: Int, milliseconds: Double) -> String {
        var metrics = Metrics()
        for verdict in Verdict.allCases {
            metrics.set(verdict.rawValue.lowercased(), records.filter { $0.outcome.verdict == verdict }.count)
        }
        metrics.set("ios", ios)
        let failed = records.contains { $0.outcome.verdict == .fail }
        return format(id: suiteID, outcome: Outcome(failed ? .fail : .pass, metrics), milliseconds: milliseconds)
    }

    public struct Parsed: Equatable {
        public let id: String
        public let verdict: Verdict
        public let fields: [String: String]

        public init(id: String, verdict: Verdict, fields: [String: String]) {
            self.id = id
            self.verdict = verdict
            self.fields = fields
        }
    }

    public static func parse(_ line: String) -> Parsed? {
        guard let start = line.range(of: prefix + " ") else { return nil }
        let parts = line[start.lowerBound...].split(separator: " ").map(String.init)
        guard parts.count >= 3, let verdict = Verdict(rawValue: parts[2]), isValidID(parts[1]) else { return nil }
        var fields: [String: String] = [:]
        for part in parts.dropFirst(3) {
            guard let equals = part.firstIndex(of: "=") else { return nil }
            fields[String(part[..<equals])] = String(part[part.index(after: equals)...])
        }
        return Parsed(id: parts[1], verdict: verdict, fields: fields)
    }
}
