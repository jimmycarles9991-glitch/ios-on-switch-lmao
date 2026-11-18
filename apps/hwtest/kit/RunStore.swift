import Foundation
import HWTestCore

/// The runs' reports in Documents/hwtest, where tools/hwtest/fetch.py finds them on the card's Data volume.
struct RunStore {
    static let kept = 40

    let directory: URL

    init(documents: URL) {
        directory = documents.appendingPathComponent("hwtest")
    }

    func save(_ report: Report) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try DurableFile.write(try report.encoded(), to: directory.appendingPathComponent(report.fileName))
    }

    /// Every report that decodes, least recently written first.
    func reports() -> [Report] {
        reportURLs().compactMap { url in (try? Data(contentsOf: url)).flatMap { try? Report.decoded($0) } }
    }

    /// Removes the oldest reports beyond `kept`; the Mac keeps every one it has fetched.
    func prune() {
        for url in reportURLs().dropLast(Self.kept) { try? FileManager.default.removeItem(at: url) }
    }

    /// Oldest first by modification time: names sort by boot UUID before time.
    private func reportURLs() -> [URL] {
        let urls =
            (try? FileManager.default.contentsOfDirectory(
                at: directory, includingPropertiesForKeys: [.contentModificationDateKey], options: .skipsHiddenFiles))
            ?? []
        let modified = { (url: URL) in
            (try? url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
        }
        return urls.filter { $0.pathExtension == "json" }.sorted { modified($0) < modified($1) }
    }
}
