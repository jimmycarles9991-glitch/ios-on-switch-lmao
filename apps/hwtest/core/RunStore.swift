import Foundation

/// The runs in AppFiles.working, where tools/hwtest/fetch.py finds them on the card's Data volume: a running run's
/// <run id>.journal (RunJournal), an ended run's <run id>.json. A run with both was reopened by a rerun, and its journal
/// is the newer.
public struct RunStore {
    public static let kept = 40

    public let directory: URL

    public init(directory: URL) {
        self.directory = directory
    }

    public func open(_ report: Report) throws -> RunJournal {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return try RunJournal(report, at: url(report.run.id, Kind.journal))
    }

    public func close(_ report: Report) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try DurableFile.write(report.encoded(), to: url(report.run.id, Kind.json))
        let journal = url(report.run.id, Kind.journal)
        if FileManager.default.fileExists(atPath: journal.path) { try FileManager.default.removeItem(at: journal) }
    }

    /// This boot's runs and the last run of an earlier boot, least recently written first: what a launch decides from
    /// (Report.interrupted, Report.lastBootStoppedIn), read without the older runs.
    public func recent(boot: String) -> [Report] {
        var found: [Report] = []
        for report in newestFirst() {
            found.append(report)
            if report.latestBootUUID != boot { break }
        }
        return found.reversed()
    }

    public func newestFirst() -> some Sequence<Report> {
        runs().lazy.compactMap(read)
    }

    /// The Mac keeps every run it has fetched.
    public func prune() {
        for run in runs().dropFirst(Self.kept) {
            for kind in run.kinds { try? FileManager.default.removeItem(at: url(run.id, kind)) }
        }
    }

    private enum Kind: String {
        case journal, json
    }

    private struct Run {
        let id: String
        var kinds: Set<Kind>
        var modified: Date
    }

    private func url(_ id: String, _ kind: Kind) -> URL {
        directory.appendingPathComponent(id + "." + kind.rawValue)
    }

    private func read(_ run: Run) -> Report? {
        if run.kinds.contains(.journal), let data = try? Data(contentsOf: url(run.id, .journal)),
           let report = RunJournal.report(from: data)
        {
            return report
        }
        guard run.kinds.contains(.json), let data = try? Data(contentsOf: url(run.id, .json)) else { return nil }
        return try? Report.decoded(data)
    }

    /// Newest first by modification time: names sort by boot UUID before time.
    private func runs() -> [Run] {
        let urls =
            (try? FileManager.default.contentsOfDirectory(
                at: directory, includingPropertiesForKeys: [.contentModificationDateKey], options: .skipsHiddenFiles
            ))
            ?? []
        var runs: [String: Run] = [:]
        for url in urls {
            guard let kind = Kind(rawValue: url.pathExtension) else { continue }
            let id = url.deletingPathExtension().lastPathComponent
            let values = try? url.resourceValues(forKeys: [.contentModificationDateKey])
            let modified = values?.contentModificationDate ?? .distantPast
            var run = runs[id] ?? Run(id: id, kinds: [], modified: modified)
            run.kinds.insert(kind)
            run.modified = max(run.modified, modified)
            runs[id] = run
        }
        return runs.values.sorted { $0.modified > $1.modified }
    }
}
