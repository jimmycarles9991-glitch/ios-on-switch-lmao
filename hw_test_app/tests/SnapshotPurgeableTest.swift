import CoreGraphics
import Darwin
import Foundation
import HWTestCore
import ImageIO

/// SplashBoard keeps an app's launch snapshot only once APFS marks its file purgeable, and boot 21's APFS refused all 99
/// marks, each on a decmpfs-compressed file with a resource fork; macOS 27's APFS refuses the mark on every decmpfs type.
/// An iOS 27 simulator's snapshots are Apple textures (com.apple.atx: ASTC 4x4 with LZFSE inside, the "lzfse 1" header
/// word) that AppleFSCompression leaves uncompressed. Whether the device's snapshots differ in format, or iOS marks a
/// compressed file under a condition this system lacks, the kernel's SwitchFsctl line decides; this test writes a
/// snapshot-sized texture that way (judged on iOS 27, where the reference is), marks it with APFS's fsctl, marks a file
/// compressed the way AppleFSCompression leaves one, and reads the snapshots SplashBoard wrote for this app.
struct SnapshotPurgeableTest: HWTest {
    let id = "system.snapshot_purgeable"
    let name = "Launch snapshots are written as APFS can mark them purgeable"
    let subsystem = Subsystem.system

    /// APFS's fsctls (what apfs.util sends): mark purgeable, _IOWR('J', 111, 48 bytes: flags, -, supplemental flags,
    /// ...); its purgeable flags, _IOR('J', 71, UInt64). 0x10200: a file, low urgency.
    static let markPurgeable: UInt = 0xc030_4a6f
    static let purgeableFlags: UInt = 0x4008_4a47
    static let fileLowUrgency: UInt64 = 0x10200
    static let width = 720
    static let height = 1280

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        let home = NSHomeDirectory()
        let directory = home + "/Library/Caches/switch-snapshot-purgeable"
        try? FileManager.default.removeItem(atPath: directory)
        try FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: directory) }
        guard let image = Self.screen() else { return .fail("could not draw the test image", metrics) }

        let types = (CGImageDestinationCopyTypeIdentifiers() as? [String] ?? []).filter {
            ["ktx", "atx", "astc"].contains(where: $0.lowercased().contains)
        }
        metrics.set("texture_types", types.isEmpty ? "-" : types.joined(separator: ","))
        let texture = directory + "/snapshot@2x.ktx"
        let written = Self.writeTexture(image, to: texture)
        let file = SnapshotFiles.describe(texture)
        metrics.merge(file.metrics, prefix: "texture_")
        let mark = Self.mark(texture)
        metrics.set("texture_mark_errno", mark)
        metrics.set("texture_purgeable_flags", String(format: "0x%llx", Self.flags(texture) ?? 0))

        let raw = directory + "/compressed.bin"
        let compressedMark = SnapshotFiles.writeCompressed(Self.pixels(image), to: raw) ? Self.mark(raw) : nil
        metrics.set("compressed_made", compressedMark != nil)
        if let compressedMark { metrics.set("compressed_mark_errno", compressedMark) }

        var own = (files: 0, compressed: 0, unmarked: 0, formats: Set<String>())
        let snapshots = home + "/Library/SplashBoard/Snapshots"
        let found = FileManager.default.enumerator(atPath: snapshots)?.allObjects as? [String] ?? []
        for relative in found {
            let path = snapshots + "/" + relative
            var info = stat()
            guard stat(path, &info) == 0, (info.st_mode & S_IFMT) == S_IFREG else { continue }
            own.files += 1
            if info.st_flags & UInt32(UF_COMPRESSED) != 0 { own.compressed += 1 }
            if (Self.flags(path) ?? 0) == 0 { own.unmarked += 1 }
            own.formats.insert(SnapshotFiles.describe(path).content)
        }
        metrics.set("own_snapshots", own.files)
        metrics.set("own_compressed", own.compressed)
        metrics.set("own_unmarked", own.unmarked)
        if !own.formats.isEmpty { metrics.set("own_formats", own.formats.sorted().joined(separator: "|")) }

        return .judged(
            [
                ("ImageIO wrote the texture", written),
                (
                    "the texture is an LZFSE Apple texture (\(file.content))",
                    file.content.hasSuffix("lzfse 1")
                        || ProcessInfo.processInfo.operatingSystemVersion.majorVersion < 26
                ),
                ("the texture is left uncompressed", !file.compressed),
                ("APFS marks the texture purgeable (errno \(mark))", mark == 0),
                ("no snapshot of this app is compressed", own.compressed == 0),
            ], metrics)
    }

    /// A snapshot-sized picture with a screen's flat regions and edges.
    static func screen() -> CGImage? {
        guard
            let context = CGContext(
                data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                space: CGColorSpace(name: CGColorSpace.sRGB)!,
                bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return nil }
        context.setFillColor(CGColor(red: 0.95, green: 0.95, blue: 0.97, alpha: 1))
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        for row in 0..<20 {
            let shade = CGFloat(row % 5) / 8
            context.setFillColor(CGColor(red: shade, green: 0.4 + shade / 2, blue: 0.9 - shade, alpha: 1))
            context.fill(CGRect(x: 24, y: 40 + row * 60, width: 48, height: 48))
            context.setFillColor(CGColor(red: 0.2, green: 0.2, blue: 0.25, alpha: 1))
            context.fill(CGRect(x: 96, y: 56 + row * 60, width: 120 + (row * 37) % 400, height: 14))
        }
        return context.makeImage()
    }

    static func pixels(_ image: CGImage) -> Data {
        image.dataProvider?.data.map { $0 as Data } ?? Data()
    }

    static func writeTexture(_ image: CGImage, to path: String) -> Bool {
        let url = URL(fileURLWithPath: path) as CFURL
        guard let destination = CGImageDestinationCreateWithURL(url, "com.apple.atx" as CFString, 1, nil) else {
            return false
        }
        CGImageDestinationAddImage(destination, image, ["kCGImagePropertyASTCUseLZFSE": true] as CFDictionary)
        return CGImageDestinationFinalize(destination)
    }

    static func mark(_ path: String) -> Int {
        var arguments = [UInt64](repeating: 0, count: 6)
        arguments[0] = fileLowUrgency
        return fsctl(path, markPurgeable, &arguments, 0) == 0 ? 0 : Int(errno)
    }

    static func flags(_ path: String) -> UInt64? {
        var value: UInt64 = 0
        return fsctl(path, purgeableFlags, &value, 0) == 0 ? value : nil
    }
}
