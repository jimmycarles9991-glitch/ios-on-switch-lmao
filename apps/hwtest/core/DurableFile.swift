import Darwin
import Foundation

/// Whole-file writes that survive a kernel panic. Darwin's fsync and Data.write(options: .atomic) leave the bytes in
/// the card's cache; only F_FULLFSYNC flushes it. The file is written beside its target and renamed over it before the
/// flush: APFS commits the rename in the same transaction the flush commits, so a reader finds the old file or the new
/// one, never a torn one.
public enum DurableFile {
    /// `staging`: where the file is written before its rename, when it must not appear beside its target half-written
    /// (a folder Spotlight indexes); on the target's volume.
    public static func write(_ data: Data, to url: URL, stagingIn staging: URL? = nil) throws {
        if let staging { try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true) }
        let directory = staging ?? url.deletingLastPathComponent()
        let staged = directory.appendingPathComponent(".\(url.lastPathComponent).new")
        let fd = open(staged.path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0o644)
        guard fd >= 0 else { throw failure("open", staged) }
        defer { close(fd) }
        try writeAll(data, fd, staged)
        guard rename(staged.path, url.path) == 0 else { throw failure("rename", url) }
        guard fcntl(fd, F_FULLFSYNC) == 0 else { throw failure("F_FULLFSYNC", url) }
    }

    /// Appends `text` and flushes it to the card before returning: a journal line that names what runs next.
    public static func append(_ text: String, to url: URL) throws {
        let fd = open(url.path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0o644)
        guard fd >= 0 else { throw failure("open", url) }
        defer { close(fd) }
        let bytes = Array(text.utf8)
        guard bytes.withUnsafeBytes({ Darwin.write(fd, $0.baseAddress!, $0.count) }) == bytes.count else {
            throw failure("write", url)
        }
        guard fcntl(fd, F_FULLFSYNC) == 0 else { throw failure("F_FULLFSYNC", url) }
    }

    /// Appends `data` to a file that exists (one that was removed stays removed). Written, the bytes outlive the
    /// process; `flush` also puts them on the card before returning, so they outlive a kernel panic.
    public static func append(_ data: Data, toExisting url: URL, flush: Bool) throws {
        let fd = open(url.path, O_WRONLY | O_APPEND | O_CLOEXEC)
        guard fd >= 0 else { throw failure("open", url) }
        defer { close(fd) }
        try writeAll(data, fd, url)
        guard !flush || fcntl(fd, F_FULLFSYNC) == 0 else { throw failure("F_FULLFSYNC", url) }
    }

    private static func writeAll(_ data: Data, _ fd: Int32, _ url: URL) throws {
        try data.withUnsafeBytes { bytes in
            var offset = 0
            while offset < bytes.count {
                let written = Darwin.write(fd, bytes.baseAddress! + offset, bytes.count - offset)
                if written < 0 && errno == EINTR { continue }
                guard written > 0 else { throw failure("write", url) }
                offset += written
            }
        }
    }

    static func failure(_ call: String, _ url: URL) -> TestFailure {
        TestFailure("\(call) \(url.lastPathComponent): \(String(cString: strerror(errno)))")
    }
}
