import Darwin
import Foundation

public enum InstallerImport {
    public static func copy(
        from url: URL, to destination: URL, progress: ((Int64, Int64) -> Void)? = nil
    ) throws {
        guard url.isFileURL else {
            throw InstallerFailure("Choose an IPA file.")
        }
        let accessed = url.startAccessingSecurityScopedResource()
        defer { if accessed { url.stopAccessingSecurityScopedResource() } }
        try FileManager.default.createDirectory(
            at: destination.deletingLastPathComponent(), withIntermediateDirectories: true
        )
        var coordinationError: NSError?
        var copyError: Error?
        var copied = false
        NSFileCoordinator().coordinate(
            readingItemAt: url, options: [], error: &coordinationError
        ) { source in
            do {
                let values = try source.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
                guard values.isRegularFile == true, (values.fileSize ?? 0) > 0 else {
                    throw InstallerFailure("Choose a complete IPA file.")
                }
                try Task.checkCancellation()
                let total = Int64(values.fileSize ?? 0)
                progress?(0, total)
                if clonefile(source.path, destination.path, 0) != 0 {
                    try stream(source, to: destination, total: total, progress: progress)
                }
                try Task.checkCancellation()
                progress?(total, total)
                copied = true
            } catch {
                copyError = error
            }
        }
        if let coordinationError { throw coordinationError }
        if let copyError { throw copyError }
        guard copied else {
            throw InstallerFailure("Files did not provide a readable copy of the IPA.")
        }
    }

    private static func stream(
        _ source: URL, to destination: URL, total: Int64, progress: ((Int64, Int64) -> Void)?
    ) throws {
        let input = try FileHandle(forReadingFrom: source)
        defer { try? input.close() }
        let fd = open(destination.path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        let output = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
        var written: Int64 = 0
        var lastUpdate = ContinuousClock.now
        while true {
            try Task.checkCancellation()
            guard let block = try input.read(upToCount: 1 << 20), !block.isEmpty else { break }
            try output.write(contentsOf: block)
            written += Int64(block.count)
            if lastUpdate.duration(to: .now) >= .milliseconds(100) {
                progress?(written, total)
                lastUpdate = .now
            }
        }
        try output.close()
        guard written == total else { throw InstallerFailure("The IPA changed during copying. Choose it again.") }
    }
}
