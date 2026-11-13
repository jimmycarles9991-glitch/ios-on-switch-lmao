import CoreGraphics
import Foundation
import HWTestCore
import ImageIO

/// NVJPG powers off 500 ms after its last decode (SwitchEngineGate) and on again at the next. A 4:2:0 photo decoded
/// after the engine sat idle longer than that, then once more at once: both on NVJPG, both matching the pattern and
/// ImageIO's software decode. The first decode after an idle power-off is the one that stalled the engine on T-v3a.
struct JPEGAfterIdleTest: HWTest {
    let id = "image.jpeg_after_idle"
    let name = "JPEG decode after NVJPG's idle power-off, and at once after it"
    let subsystem = Subsystem.images
    static let idleSeconds = 1.2

    func run(_ context: TestContext) async throws -> Outcome {
        guard let still = StillImage.all.first(where: { $0.format == .jpeg420 }) else {
            return .skip("no 4:2:0 JPEG in the build's stills")
        }
        let data = try Data(contentsOf: context.media.appendingPathComponent(still.file))
        guard let software = Self.decode(data, hardware: false) else {
            return .fail("ImageIO's software decode failed")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        try await Task.sleep(nanoseconds: UInt64(Self.idleSeconds * 1e9))
        for pass in ["after_idle", "powered"] {
            let before = JPEGRoute.decodes()
            let start = CFAbsoluteTimeGetCurrent()
            let decoded = Self.decode(data, hardware: true)
            metrics.set("\(pass)_ms", (CFAbsoluteTimeGetCurrent() - start) * 1000, digits: 1)
            let after = JPEGRoute.decodes()
            switch (before, after) {
            case (.success(let first), .success(let last)):
                metrics.set("\(pass)_route", last.decodes > first.decodes ? "nvjpg" : "cpu")
                checks.append(("\(pass): decoded on NVJPG", last.decodes > first.decodes))
            case (.failure(let why), _), (_, .failure(let why)):
                metrics.set("\(pass)_route", "unknown")
                checks.append(("\(pass): decoded on NVJPG (route unreadable: \(why.description))", false))
            }
            guard let decoded, let d = Pixels.difference(decoded, software) else {
                checks.append(("\(pass): decodes at the software decode's size", false))
                continue
            }
            metrics.set("\(pass)_vs_software_mean", d.mean, digits: 2)
            checks.append(
                (
                    "\(pass): matches the pattern",
                    Pixels.check(decoded, pattern: still.pattern).matches(
                        frame: 0, tolerance: still.tolerance)
                ))
            checks.append(("\(pass): within 1.5 mean of the software decode", d.mean <= 1.5))
        }
        metrics.set("idle_ms", Int(Self.idleSeconds * 1000))
        return .judged(checks, metrics)
    }

    static func decode(_ data: Data, hardware: Bool) -> CGImage? {
        guard let source = CGImageSourceCreateWithData(data as CFData, nil) else { return nil }
        let options: [CFString: Any] = [
            kCGImageSourceShouldCacheImmediately: true, ImageDecodeTest.hardwareKey: hardware,
        ]
        return CGImageSourceCreateImageAtIndex(source, 0, options as CFDictionary)
    }
}
