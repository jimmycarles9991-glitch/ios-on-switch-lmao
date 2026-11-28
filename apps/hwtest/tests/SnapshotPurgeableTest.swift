import CoreGraphics
import Darwin
import Foundation
import HWTestCore
import HWTestKit
import HWTestShared
import ImageIO

/// SplashBoard keeps an app's launch snapshot only once APFS marks its file purgeable. An app's sandbox denies it the
/// mark and the flags read (`deny(1) system-fsctl`), so the test judges the marks the system asked for since boot
/// (PurgeableMarks, debug kernels), one check per cause:
/// - a Data volume without the purgeable feature bits (ENOTSUP; cards imaged before 34c147a35);
/// - SplashBoard's compressed snapshots, which APFS refuses.
/// Reported besides: this app's own snapshots (count, compressed) and the Apple texture ImageIO writes for a
/// snapshot-sized picture (SplashBoard's are `lzfse 0`, ImageIO's `lzfse 1`).
struct SnapshotPurgeableTest: HWTest {
    let id = "system.snapshot_purgeable"
    let name = "APFS marks launch snapshots purgeable"
    let subsystem = Subsystem.system

    static let width = 720
    static let height = 1280

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        let home = NSHomeDirectory()
        let directory = home + "/Library/Caches/switch-snapshot-purgeable"
        try? FileManager.default.removeItem(atPath: directory)
        try FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: directory) }
        if let image = Self.screen() {
            let texture = directory + "/snapshot@2x.ktx"
            metrics.set("texture_written", Self.writeTexture(image, to: texture))
            metrics.merge(SnapshotFiles.describe(texture).metrics, prefix: "texture_")
        }

        var own = (files: 0, compressed: 0, formats: Set<String>())
        let snapshots = home + "/Library/SplashBoard/Snapshots"
        let found = FileManager.default.enumerator(atPath: snapshots)?.allObjects as? [String] ?? []
        for relative in found {
            let path = snapshots + "/" + relative
            var info = stat()
            guard stat(path, &info) == 0, (info.st_mode & S_IFMT) == S_IFREG else { continue }
            own.files += 1
            if info.st_flags & UInt32(UF_COMPRESSED) != 0 { own.compressed += 1 }
            own.formats.insert(SnapshotFiles.describe(path).content)
        }
        metrics.set("own_snapshots", own.files)
        metrics.set("own_compressed", own.compressed)
        if !own.formats.isEmpty { metrics.set("own_formats", own.formats.sorted().joined(separator: "|")) }

        let read = KernelSysctl.string("kern.switch_fsctl_marks")
        guard case .value(let line) = read, let marks = PurgeableMarks.parse(line) else {
            metrics.set("sysctl", KernelSysctl.code(read.error))
            return .skip(KernelSysctl.unreadable("kern.switch_fsctl_marks", read.error), metrics)
        }
        metrics.set("marks_ok", marks.ok)
        metrics.set("marks_unsupported", marks.unsupported)
        metrics.set("marks_compressed", marks.compressed)
        metrics.set("marks_other", marks.other)
        for (cause, name) in marks.by.sorted(by: { $0.key < $1.key }) where name != "-" { metrics.set(
            "marks_\(cause)_by",
            name
        ) }
        guard marks.asked > 0 else {
            return .skip("no process asked APFS for a purgeable mark since boot", metrics)
        }
        return .judged(marks.checks, metrics)
    }

    /// A snapshot-sized picture with a screen's flat regions and edges.
    static func screen() -> CGImage? {
        guard
            let context = CGContext(
                data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                space: CGColorSpace(name: CGColorSpace.sRGB)!,
                bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
            )
        else { return nil }
        context.setFillColor(CGColor(red: 0.95, green: 0.95, blue: 0.97, alpha: 1))
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        for row in 0 ..< 20 {
            let shade = CGFloat(row % 5) / 8
            context.setFillColor(CGColor(red: shade, green: 0.4 + shade / 2, blue: 0.9 - shade, alpha: 1))
            context.fill(CGRect(x: 24, y: 40 + row * 60, width: 48, height: 48))
            context.setFillColor(CGColor(red: 0.2, green: 0.2, blue: 0.25, alpha: 1))
            context.fill(CGRect(x: 96, y: 56 + row * 60, width: 120 + (row * 37) % 400, height: 14))
        }
        return context.makeImage()
    }

    static func writeTexture(_ image: CGImage, to path: String) -> Bool {
        let url = URL(fileURLWithPath: path) as CFURL
        guard let destination = CGImageDestinationCreateWithURL(url, "com.apple.atx" as CFString, 1, nil) else {
            return false
        }
        CGImageDestinationAddImage(destination, image, ["kCGImagePropertyASTCUseLZFSE": true] as CFDictionary)
        return CGImageDestinationFinalize(destination)
    }
}
