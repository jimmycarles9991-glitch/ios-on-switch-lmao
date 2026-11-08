import Foundation

public enum Verdict: String, Codable, Sendable, CaseIterable {
    case pass = "PASS"
    case fail = "FAIL"
    case skip = "SKIP"
}

public enum MetricValue: Codable, Equatable, Sendable {
    case integer(Int)
    case number(Double, digits: Int)
    case text(String)
    case flag(Bool)

    public var logText: String {
        switch self {
        case .integer(let value): return String(value)
        case .number(let value, let digits): return Self.format(value, digits: digits)
        case .text(let value): return LogLine.sanitized(value)
        case .flag(let value): return value ? "1" : "0"
        }
    }

    static func format(_ value: Double, digits: Int) -> String {
        guard value.isFinite else { return value.isNaN ? "nan" : (value > 0 ? "inf" : "-inf") }
        return String(format: "%.\(max(0, digits))f", value)
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .integer(let value): try container.encode(value)
        case .number(let value, let digits):
            if value.isFinite {
                try container.encode(Double(Self.format(value, digits: digits)) ?? value)
            } else {
                try container.encode(Self.format(value, digits: digits))
            }
        case .text(let value): try container.encode(value)
        case .flag(let value): try container.encode(value)
        }
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if let value = try? container.decode(Bool.self) {
            self = .flag(value)
        } else if let value = try? container.decode(Int.self) {
            self = .integer(value)
        } else if let value = try? container.decode(Double.self) {
            self = .number(value, digits: 3)
        } else {
            self = .text(try container.decode(String.self))
        }
    }
}

/// Ordered key/value measurements; the log line keeps insertion order so its fields stay where readers expect them.
public struct Metrics: Equatable, Sendable {
    public private(set) var keys: [String] = []
    public private(set) var values: [String: MetricValue] = [:]

    public init() {}

    public subscript(key: String) -> MetricValue? { values[key] }

    public mutating func set(_ key: String, _ value: MetricValue) {
        precondition(LogLine.isValidKey(key) && !LogLine.reservedKeys.contains(key), "bad metric key \(key)")
        if values.updateValue(value, forKey: key) == nil { keys.append(key) }
    }

    public mutating func set(_ key: String, _ value: Int) { set(key, .integer(value)) }
    public mutating func set(_ key: String, _ value: Double, digits: Int = 2) {
        set(key, .number(value, digits: digits))
    }
    public mutating func set(_ key: String, _ value: String) { set(key, .text(value)) }
    public mutating func set(_ key: String, _ value: Bool) { set(key, .flag(value)) }

    public mutating func merge(_ other: Metrics, prefix: String = "") {
        for key in other.keys { set(prefix + key, other.values[key]!) }
    }

    public var ordered: [(String, MetricValue)] { keys.map { ($0, values[$0]!) } }
}

/// What one test reports: its verdict, measurements, why it failed or was skipped, and the evidence that the
/// hardware path ran (a property the API reads back, a GPU timestamp, a decoder name) where the API exposes one.
public struct Outcome: Sendable {
    public var verdict: Verdict
    public var metrics: Metrics
    public var detail: String
    public var proof: String

    public init(_ verdict: Verdict, _ metrics: Metrics = Metrics(), detail: String = "", proof: String = "") {
        self.verdict = verdict
        self.metrics = metrics
        self.detail = detail
        self.proof = proof
    }

    public static func pass(_ metrics: Metrics = Metrics(), proof: String = "", detail: String = "") -> Outcome {
        Outcome(.pass, metrics, detail: detail, proof: proof)
    }

    public static func fail(_ detail: String, _ metrics: Metrics = Metrics(), proof: String = "") -> Outcome {
        Outcome(.fail, metrics, detail: detail, proof: proof)
    }

    public static func skip(_ detail: String, _ metrics: Metrics = Metrics()) -> Outcome {
        Outcome(.skip, metrics, detail: detail)
    }

    /// PASS when every check holds, otherwise FAIL naming the checks that did not.
    public static func judged(
        _ checks: [(String, Bool)], _ metrics: Metrics, proof: String = "", detail: String = ""
    ) -> Outcome {
        let failed = checks.filter { !$0.1 }.map(\.0)
        if failed.isEmpty { return .pass(metrics, proof: proof, detail: detail) }
        return .fail(failed.joined(separator: "; "), metrics, proof: proof)
    }
}

public struct TestFailure: Error, CustomStringConvertible {
    public let description: String

    public init(_ description: String) { self.description = description }
}

public struct TestRecord: Sendable {
    public let id: String
    public let subsystem: String
    public let name: String
    public let interactive: Bool
    public let outcome: Outcome
    public let milliseconds: Double
    public let finished: Date

    public init(
        id: String, subsystem: String, name: String, interactive: Bool, outcome: Outcome, milliseconds: Double,
        finished: Date
    ) {
        self.id = id
        self.subsystem = subsystem
        self.name = name
        self.interactive = interactive
        self.outcome = outcome
        self.milliseconds = milliseconds
        self.finished = finished
    }
}
