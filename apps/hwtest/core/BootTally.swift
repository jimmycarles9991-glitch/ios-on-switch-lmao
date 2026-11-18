import Foundation

/// A count of this boot's events, kept in a file of the app's so that a later process of the same boot (Run All
/// resumed after a crash) reads what an earlier one added. The file holds "<boot> <count>"; another boot's reads 0.
/// Tests run one at a time, so one process reads and writes it at a time.
public struct BootTally {
    let url: URL
    let boot: String

    public init(url: URL, boot: String) {
        self.url = url
        self.boot = boot
    }

    public var count: Int {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return 0 }
        let parts = text.split(separator: " ")
        guard parts.count == 2, parts[0] == boot, let count = Int(parts[1]) else { return 0 }
        return count
    }

    public func add() throws {
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try DurableFile.write(Data("\(boot) \(count + 1)".utf8), to: url)
    }
}
