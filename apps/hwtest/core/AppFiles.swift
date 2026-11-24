import Darwin
import Foundation

/// Where HW Test's files go. Documents is the app's folder in Files (UIFileSharingEnabled and
/// LSSupportsOpeningDocumentsInPlace), which Spotlight's file indexer follows: it holds only finished things a person
/// opens (the tests' pictures, the bench loop's results), and each appears there whole, by a rename. Everything
/// rewritten while a run goes (the run's journal and report, the probes' steps, the boot's tallies) lives in
/// Library/Application Support/hwtest, outside the index.
public enum AppFiles {
    public static let working: URL = {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return support.appendingPathComponent("hwtest")
    }()

    public static let staging = working.appendingPathComponent("staging")

    /// `data` written whole beside the working files, then renamed to `url` (in the same container: one volume).
    public static func place(_ data: Data, at url: URL, staging: URL = staging) throws {
        try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(), withIntermediateDirectories: true
        )
        let staged = staging.appendingPathComponent(UUID().uuidString)
        try data.write(to: staged)
        guard rename(staged.path, url.path) == 0 else {
            let failure = DurableFile.failure("rename", url)
            try? FileManager.default.removeItem(at: staged)
            throw failure
        }
    }
}

extension Data {
    public func place(at url: URL) throws {
        try AppFiles.place(self, at: url)
    }
}
