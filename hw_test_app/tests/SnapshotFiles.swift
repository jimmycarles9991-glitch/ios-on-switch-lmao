import Compression
import Darwin
import Foundation
import HWTestCore

/// A file as APFS holds it (BSD flags, decmpfs header, resource fork) and as a reader sees its first bytes, and a file
/// compressed the way AppleFSCompression leaves one: LZFSE in 64 KiB chunks in the resource fork (decmpfs type 12).
enum SnapshotFiles {
    struct Description {
        var content: String
        var compressed: Bool
        var metrics: Metrics
    }

    static let chunk = 65536
    static let lzfseResourceFork: UInt32 = 12

    static func describe(_ path: String) -> Description {
        var metrics = Metrics()
        var info = stat()
        guard stat(path, &info) == 0 else {
            return Description(content: "missing", compressed: false, metrics: metrics)
        }
        let compressed = info.st_flags & UInt32(UF_COMPRESSED) != 0
        metrics.set("bytes", Int(info.st_size))
        metrics.set("st_flags", String(format: "0x%x", info.st_flags))
        var header = [UInt8](repeating: 0, count: 16)
        let decmpfs = getxattr(path, "com.apple.decmpfs", &header, header.count, 0, XATTR_SHOWCOMPRESSION)
        metrics.set("decmpfs_type", decmpfs >= 16 ? Int(le32(header, 4)) : 0)
        metrics.set("resource_fork", max(0, getxattr(path, "com.apple.ResourceFork", nil, 0, 0, XATTR_SHOWCOMPRESSION)))
        let head = FileHandle(forReadingAtPath: path).flatMap { try? $0.read(upToCount: 0x48) }.map([UInt8].init) ?? []
        let content = describeContent(head)
        metrics.set("format", content)
        return Description(content: content, compressed: compressed, metrics: metrics)
    }

    /// As the kernel's SwitchFsctl line says it (<switch/fsctl_diag.h>).
    static func describeContent(_ b: [UInt8]) -> String {
        if b.count >= 0x48, b.prefix(8) == [0x41, 0x41, 0x50, 0x4c, 0x0d, 0x0a, 0x1a, 0x0a] {
            return String(
                format: "AAPL texture format 0x%x %ux%u lzfse %u", le32(b, 0x20), le32(b, 0x28), le32(b, 0x2c),
                le32(b, 0x44))
        }
        if b.count >= 0x2c, b.prefix(8) == [0xab, 0x4b, 0x54, 0x58, 0x20, 0x31, 0x31, 0xbb] {
            return String(
                format: "KTX 1 texture internal format 0x%x %ux%u", le32(b, 0x1c), le32(b, 0x24), le32(b, 0x28))
        }
        return b.prefix(8).map { String(format: "%02x", $0) }.joined()
    }

    static func le32(_ b: [UInt8], _ at: Int) -> UInt32 {
        UInt32(b[at]) | UInt32(b[at + 1]) << 8 | UInt32(b[at + 2]) << 16 | UInt32(b[at + 3]) << 24
    }

    static func writeCompressed(_ data: Data, to path: String) -> Bool {
        guard !data.isEmpty, FileManager.default.createFile(atPath: path, contents: nil) else { return false }
        var chunks: [Data] = []
        for start in stride(from: 0, to: data.count, by: chunk) {
            let source = [UInt8](data[start..<min(start + chunk, data.count)])
            var out = [UInt8](repeating: 0, count: source.count + 4096)
            let size = compression_encode_buffer(&out, out.count, source, source.count, nil, COMPRESSION_LZFSE)
            guard size > 0 else { return false }
            chunks.append(Data(out.prefix(size)))
        }
        var fork = Data()
        var offset = UInt32(4 * (chunks.count + 1))
        for piece in chunks + [Data()] {
            withUnsafeBytes(of: offset.littleEndian) { fork.append(contentsOf: $0) }
            offset += UInt32(piece.count)
        }
        chunks.forEach { fork.append($0) }
        var header = Data("fpmc".utf8)
        withUnsafeBytes(of: lzfseResourceFork.littleEndian) { header.append(contentsOf: $0) }
        withUnsafeBytes(of: UInt64(data.count).littleEndian) { header.append(contentsOf: $0) }
        let set = { (name: String, value: Data) in
            value.withUnsafeBytes { setxattr(path, name, $0.baseAddress, value.count, 0, XATTR_SHOWCOMPRESSION) == 0 }
        }
        return set("com.apple.ResourceFork", fork) && set("com.apple.decmpfs", header)
            && chflags(path, UInt32(UF_COMPRESSED)) == 0
    }
}
