import Foundation

/// The JSON report the app keeps in its Documents folder (report.json), rewritten after every test so an interrupted
/// run still leaves what it measured.
public struct Report: Codable, Equatable {
    public struct Entry: Codable, Equatable {
        public let id: String
        public let subsystem: String
        public let name: String
        public let verdict: Verdict
        public let metrics: [String: MetricValue]
        public let proof: String
        public let detail: String
        public let milliseconds: Double
        public let interactive: Bool
        public let finished: String
        public let log: String
    }

    public struct Summary: Codable, Equatable {
        public let pass: Int
        public let fail: Int
        public let skip: Int

        public init(pass: Int, fail: Int, skip: Int) {
            self.pass = pass
            self.fail = fail
            self.skip = skip
        }
    }

    public static let schema = 1

    public let schema: Int
    public let started: String
    public let device: [String: String]
    public let summary: Summary
    public let results: [Entry]

    public init(started: Date, device: [String: String], records: [TestRecord]) {
        let latest = Self.latestPerTest(records)
        schema = Self.schema
        self.started = Self.timestamp(started)
        self.device = device
        results = latest.map { record in
            Entry(
                id: record.id,
                subsystem: record.subsystem,
                name: record.name,
                verdict: record.outcome.verdict,
                metrics: record.outcome.metrics.values,
                proof: record.outcome.proof,
                detail: record.outcome.detail,
                milliseconds: (record.milliseconds * 10).rounded() / 10,
                interactive: record.interactive,
                finished: Self.timestamp(record.finished),
                log: LogLine.format(record)
            )
        }
        summary = Summary(
            pass: latest.filter { $0.outcome.verdict == .pass }.count,
            fail: latest.filter { $0.outcome.verdict == .fail }.count,
            skip: latest.filter { $0.outcome.verdict == .skip }.count
        )
    }

    /// A rerun replaces the test's earlier record in place.
    static func latestPerTest(_ records: [TestRecord]) -> [TestRecord] {
        var order: [String] = []
        var latest: [String: TestRecord] = [:]
        for record in records {
            if latest.updateValue(record, forKey: record.id) == nil { order.append(record.id) }
        }
        return order.map { latest[$0]! }
    }

    public func encoded() throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return try encoder.encode(self)
    }

    static func timestamp(_ date: Date) -> String {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return formatter.string(from: date)
    }
}
