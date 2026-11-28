import Foundation

/// A run on the card while it goes: Library/Application Support/hwtest/<run id>.journal (AppFiles.working). The first
/// line is the whole report as the batch
/// started; each later line is one change, appended as it happens: a test begun, a test's result, the app going away
/// or coming back, the run finished. Lines are JSON without newlines inside. Each append is one write of whole lines,
/// so a process that dies mid-write leaves at most a torn last line, which a read leaves out. A test's result and the
/// next test's begin go to the card together, flushed (one F_FULLFSYNC a test), before that next test starts, so a
/// kernel panic in a test still names it. The finished run is written once as <run id>.json and its journal removed
/// (RunStore.close).
public final class RunJournal {
    public enum Change: Equatable {
        case begin(String)
        case result(Report.Entry)
        case interruption(String?)
        case finished(String)
    }

    public let url: URL
    public private(set) var report: Report

    public init(_ report: Report, at url: URL) throws {
        self.url = url
        self.report = report
        try DurableFile.write(Self.encoder.encode(report) + Self.newline, to: url)
    }

    /// `finished`'s result and `next`'s begin, on the card before this returns.
    public func advance(_ finished: TestRecord?, begin next: String?) throws {
        var changes: [Change] = []
        if let finished { changes.append(.result(report.entry(finished))) }
        if let next { changes.append(.begin(next)) }
        try append(changes, flush: true)
    }

    /// Written, not flushed: the line outlives the process, and after a restart the running test is requeued whatever
    /// the line says.
    public func interrupt(_ reason: String?) throws {
        guard report.run.interruption != reason else { return }
        try append([.interruption(reason)], flush: false)
    }

    public func finish(at date: Date) throws {
        try append([.finished(Report.timestamp(date))], flush: true)
    }

    func append(_ changes: [Change], flush: Bool) throws {
        guard !changes.isEmpty else { return }
        var data = Data()
        for change in changes { data += try Self.encoder.encode(Line(change)) + Self.newline }
        try DurableFile.append(data, toExisting: url, flush: flush)
        for change in changes { report.apply(change) }
    }

    /// Reading stops at a line without its newline (the process died writing it) or one that does not decode.
    public static func report(from data: Data) -> Report? {
        var lines = data.split(separator: newline[0], omittingEmptySubsequences: false)
        _ = lines.popLast()
        guard let first = lines.first, var report = try? Report.decoded(Data(first)) else { return nil }
        let decoder = JSONDecoder()
        for line in lines.dropFirst() {
            guard let change = (try? decoder.decode(Line.self, from: Data(line)))?.change else { break }
            report.apply(change)
        }
        return report
    }

    private static let newline = Data("\n".utf8)

    private static let encoder: JSONEncoder = {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return encoder
    }()

    /// A cleared interruption is "".
    private struct Line: Codable {
        var begin: String?
        var result: Report.Entry?
        var interruption: String?
        var finished: String?

        init(_ change: Change) {
            switch change {
            case .begin(let id): begin = id
            case .result(let entry): result = entry
            case .interruption(let reason): interruption = reason ?? ""
            case .finished(let at): finished = at
            }
        }

        var change: Change? {
            if let begin { return .begin(begin) }
            if let result { return .result(result) }
            if let interruption { return .interruption(interruption.isEmpty ? nil : interruption) }
            if let finished { return .finished(finished) }
            return nil
        }
    }
}

extension Report {
    mutating func apply(_ change: RunJournal.Change) {
        switch change {
        case .begin(let id): begin(id)
        case .result(let entry): record(entry)
        case .interruption(let reason): interrupt(reason)
        case .finished(let at): finish(timestamp: at)
        }
    }
}
