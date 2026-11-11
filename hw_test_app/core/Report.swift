import Foundation

/// One run's JSON report, Documents/hwtest/<boot uuid>-<stamp>.json. It is written durably before each test
/// (`run.current` names the test) and after it, so a run that the app or the OS did not survive still holds every
/// finished result and the test that was running. tools/hwtest reads it on the Mac.
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
        public let bootUUID: String?

        enum CodingKeys: String, CodingKey {
            case id, subsystem, name, verdict, metrics, proof, detail, milliseconds, interactive, finished, log
            case bootUUID = "boot_uuid"
        }

        public init(_ record: TestRecord, bootUUID: String? = nil) {
            id = record.id
            subsystem = record.subsystem
            name = record.name
            verdict = record.outcome.verdict
            metrics = record.outcome.metrics.values
            proof = record.outcome.proof
            detail = record.outcome.detail
            milliseconds = (record.milliseconds * 10).rounded() / 10
            interactive = record.interactive
            finished = Report.timestamp(record.finished)
            log = LogLine.format(record)
            self.bootUUID = bootUUID
        }

        public var crashed: Bool { metrics[Report.crashedKey] == .flag(true) }
    }

    public enum Status: String, Codable, Sendable {
        case running, done, cancelled
    }

    public struct Resumption: Codable, Equatable {
        public let bootUUID: String
        public let at: String
        public let identity: [String: String]

        enum CodingKeys: String, CodingKey {
            case at, identity
            case bootUUID = "boot_uuid"
        }
    }

    public struct Run: Codable, Equatable {
        public let id: String
        public let bootUUID: String
        public let ios: Int
        public let started: String
        public var status: Status
        /// The test that started and has not finished: after a crash, the one that was running.
        public var current: String?
        public var finished: String?
        /// Every test the run means to finish, in run order.
        public var order: [String]
        /// Set while the app is in the background or closing ("backgrounded", "closed"): a process that ends then was
        /// ended by the person or by the system, not by the test.
        public var interruption: String?

        enum CodingKeys: String, CodingKey {
            case id, ios, started, status, current, finished, order, interruption
            case bootUUID = "boot_uuid"
        }
    }

    public struct Summary: Codable, Equatable {
        public let pass: Int
        public let fail: Int
        public let skip: Int
        public let crashed: Int
        public let planned: Int

        public init(pass: Int, fail: Int, skip: Int, crashed: Int = 0, planned: Int = 0) {
            self.pass = pass
            self.fail = fail
            self.skip = skip
            self.crashed = crashed
            self.planned = planned
        }
    }

    public static let schema = 2
    /// The metric a test's entry carries when its process ended while it ran.
    public static let crashedKey = "crashed"

    public let schema: Int
    public private(set) var run: Run
    /// What was running: iOS build, kernel, boot, the app's own image (DeviceInfo.identity).
    public let identity: [String: String]
    public private(set) var summary: Summary
    public private(set) var results: [Entry]
    public private(set) var resumptions: [Resumption]?

    public init(bootUUID: String, ios: Int, started: Date, identity: [String: String]) {
        schema = Self.schema
        let stamp = Self.stamp(started)
        run = Run(
            id: "\(bootUUID)-\(stamp)", bootUUID: bootUUID, ios: ios, started: Self.timestamp(started), status: .done,
            current: nil, finished: nil, order: [], interruption: nil)
        self.identity = identity
        results = []
        summary = Summary(pass: 0, fail: 0, skip: 0)
    }

    /// The run's file name: its id, which sorts by boot, then by start time.
    public var fileName: String { run.id + ".json" }

    public var latestBootUUID: String { resumptions?.last?.bootUUID ?? run.bootUUID }

    public func bootUUID(for entry: Entry) -> String { entry.bootUUID ?? run.bootUUID }

    public var remaining: [String] {
        let finished = Set(results.map(\.id))
        return run.order.filter { !finished.contains($0) && $0 != run.current }
    }

    /// Adds `ids` the run has not planned yet and marks it running.
    public mutating func plan(_ ids: [String]) {
        let planned = Set(run.order)
        run.order += ids.filter { !planned.contains($0) }
        run.status = .running
        run.finished = nil
        summarize()
    }

    public mutating func begin(_ id: String) {
        run.current = id
    }

    public mutating func interrupt(_ reason: String?) {
        run.interruption = reason
    }

    /// Puts the test an interruption cut short back among the remaining ones, to run again.
    public mutating func requeueInterrupted() {
        if let current = run.current { results.removeAll { $0.id == current } }
        run.current = nil
        run.interruption = nil
        summarize()
    }

    /// A test's record; a rerun replaces its earlier entry in place.
    public mutating func record(_ record: TestRecord) {
        let entry = Entry(record, bootUUID: latestBootUUID)
        if let index = results.firstIndex(where: { $0.id == entry.id }) {
            results[index] = entry
        } else {
            results.append(entry)
        }
        if run.current == entry.id { run.current = nil }
        summarize()
    }

    public mutating func finish(at date: Date) {
        run.status = .done
        run.current = nil
        run.interruption = nil
        run.finished = Self.timestamp(date)
    }

    public mutating func cancel(at date: Date) {
        finish(at: date)
        run.status = .cancelled
    }

    public mutating func resume(bootUUID: String, at date: Date, identity: [String: String]) {
        guard run.status == .running, bootUUID != latestBootUUID else { return }
        resumptions =
            (resumptions ?? []) + [Resumption(bootUUID: bootUUID, at: Self.timestamp(date), identity: identity)]
    }

    /// `reports`: least recently written first. A newer finished or cancelled run supersedes older unfinished ones.
    public static func interrupted(_ reports: [Report]) -> Report? {
        reports.last.flatMap { $0.run.status == .running ? $0 : nil }
    }

    /// The test the last run of an earlier boot was in when that boot ended: the OS went down while it ran, so the
    /// next run puts it last and still finishes everything else. `reports`: least recently written first.
    public static func lastBootStoppedIn(_ reports: [Report], boot: String) -> String? {
        guard let last = reports.last(where: { $0.latestBootUUID != boot }), last.run.status == .running else {
            return nil
        }
        return last.run.current
    }

    /// FAIL for a test whose process ended while it ran, with what the next launch knows of it.
    public static func crashOutcome(_ id: String) -> Outcome {
        var metrics = Metrics()
        metrics.set(crashedKey, true)
        return .fail("the app's process ended while \(id) ran (crash, jetsam or a kill: see its .ips)", metrics)
    }

    mutating func summarize() {
        let results = results
        let count = { (verdict: Verdict) in results.filter { $0.verdict == verdict }.count }
        summary = Summary(
            pass: count(.pass), fail: count(.fail), skip: count(.skip), crashed: results.filter(\.crashed).count,
            planned: run.order.count)
    }

    public func encoded() throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return try encoder.encode(self)
    }

    public static func decoded(_ data: Data) throws -> Report {
        try JSONDecoder().decode(Report.self, from: data)
    }

    public static func timestamp(_ date: Date) -> String {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return formatter.string(from: date)
    }

    static func stamp(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        return formatter.string(from: date)
    }
}

extension Outcome {
    /// An entry read back from a report, for the list: its metrics in key order.
    public init(_ entry: Report.Entry) {
        var metrics = Metrics()
        for key in entry.metrics.keys.sorted() where LogLine.isValidKey(key) && !LogLine.reservedKeys.contains(key) {
            metrics.set(key, entry.metrics[key]!)
        }
        self.init(entry.verdict, metrics, detail: entry.detail, proof: entry.proof)
    }
}
