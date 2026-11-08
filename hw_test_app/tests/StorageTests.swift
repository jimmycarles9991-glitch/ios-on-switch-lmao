import Darwin
import Foundation
import HWTestCore

/// File IO in the app's sandbox with F_NOCACHE, so the numbers are the SD card and its driver, not the page cache.
enum Disk {
    static func open(_ url: URL, write: Bool) throws -> Int32 {
        let fd = Darwin.open(url.path, write ? (O_RDWR | O_CREAT | O_TRUNC) : O_RDONLY, 0o644)
        guard fd >= 0 else { throw TestFailure("open \(url.lastPathComponent): \(String(cString: strerror(errno)))") }
        _ = fcntl(fd, F_NOCACHE, 1)
        return fd
    }

    static func block(_ size: Int, seed: Int) -> [UInt8] {
        (0..<size).map { UInt8(truncatingIfNeeded: ($0 &+ seed) &* 2_654_435_761 >> 13) }
    }

    static func file(_ name: String) -> URL {
        FileManager.default.temporaryDirectory.appendingPathComponent(name)
    }
}

struct SequentialStorageTest: HWTest {
    let id = "storage.sequential"
    let name = "Sequential read and write"
    let subsystem = Subsystem.storage
    static let chunk = 1 << 20
    static let chunks = 128

    func run(_ context: TestContext) async throws -> Outcome {
        let url = Disk.file("sequential.bin")
        defer { try? FileManager.default.removeItem(at: url) }
        let blocks = (0..<4).map { Disk.block(Self.chunk, seed: $0) }
        var fd = try Disk.open(url, write: true)
        var start = CFAbsoluteTimeGetCurrent()
        for index in 0..<Self.chunks {
            guard blocks[index % 4].withUnsafeBytes({ write(fd, $0.baseAddress, Self.chunk) }) == Self.chunk else {
                close(fd)
                throw TestFailure("short write at chunk \(index)")
            }
        }
        fsync(fd)
        let writeSeconds = CFAbsoluteTimeGetCurrent() - start
        close(fd)
        fd = try Disk.open(url, write: false)
        defer { close(fd) }
        var buffer = [UInt8](repeating: 0, count: Self.chunk)
        var wrong = 0
        start = CFAbsoluteTimeGetCurrent()
        for index in 0..<Self.chunks {
            guard buffer.withUnsafeMutableBytes({ read(fd, $0.baseAddress, Self.chunk) }) == Self.chunk else {
                throw TestFailure("short read at chunk \(index)")
            }
            if buffer != blocks[index % 4] { wrong += 1 }
        }
        let readSeconds = CFAbsoluteTimeGetCurrent() - start
        let bytes = Self.chunk * Self.chunks
        var metrics = Metrics()
        metrics.set("write_mbps", Stats.megabytesPerSecond(bytes: bytes, seconds: writeSeconds), digits: 1)
        metrics.set("read_mbps", Stats.megabytesPerSecond(bytes: bytes, seconds: readSeconds), digits: 1)
        metrics.set("mb", bytes >> 20)
        return .judged([("every chunk reads back", wrong == 0)], metrics)
    }
}

struct RandomStorageTest: HWTest {
    let id = "storage.random"
    let name = "Random 4 KiB read and write"
    let subsystem = Subsystem.storage
    static let size = 64 << 20
    static let page = 4096

    func run(_ context: TestContext) async throws -> Outcome {
        let url = Disk.file("random.bin")
        defer { try? FileManager.default.removeItem(at: url) }
        let fd = try Disk.open(url, write: true)
        defer { close(fd) }
        let pattern = Disk.block(1 << 20, seed: 7)
        for offset in stride(from: 0, to: Self.size, by: 1 << 20) {
            _ = pattern.withUnsafeBytes { pwrite(fd, $0.baseAddress, 1 << 20, off_t(offset)) }
        }
        fsync(fd)
        var generator = SystemRandomNumberGenerator()
        var buffer = [UInt8](repeating: 0, count: Self.page)
        var (reads, wrong) = (0, 0)
        var start = CFAbsoluteTimeGetCurrent()
        while CFAbsoluteTimeGetCurrent() - start < 2 {
            let page = Int.random(in: 0..<(Self.size / Self.page), using: &generator)
            _ = buffer.withUnsafeMutableBytes { pread(fd, $0.baseAddress, Self.page, off_t(page * Self.page)) }
            let at = (page * Self.page) % (1 << 20)
            if buffer[...] != pattern[at..<(at + Self.page)] { wrong += 1 }
            reads += 1
        }
        let readIOPS = Double(reads) / (CFAbsoluteTimeGetCurrent() - start)
        var writes = 0
        start = CFAbsoluteTimeGetCurrent()
        while CFAbsoluteTimeGetCurrent() - start < 2 {
            let page = Int.random(in: 0..<(Self.size / Self.page), using: &generator)
            _ = buffer.withUnsafeBytes { pwrite(fd, $0.baseAddress, Self.page, off_t(page * Self.page)) }
            writes += 1
            if writes % 64 == 0 { fsync(fd) }
        }
        fsync(fd)
        let writeIOPS = Double(writes) / (CFAbsoluteTimeGetCurrent() - start)
        var metrics = Metrics()
        metrics.set("read_iops", readIOPS, digits: 0)
        metrics.set("write_iops", writeIOPS, digits: 0)
        return .judged([("random reads return the written data", wrong == 0 && reads > 0)], metrics)
    }
}
