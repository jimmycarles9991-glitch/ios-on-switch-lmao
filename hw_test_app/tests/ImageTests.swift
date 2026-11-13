import CoreGraphics
import CoreImage
import Foundation
import HWTestCore
import ImageIO
import Metal

enum Pixels {
    /// Draws a decoded image into an RGBX buffer and checks it against the pattern.
    static func check(_ image: CGImage, pattern: Pattern) -> FrameCheck {
        let context = CGContext(
            data: nil, width: image.width, height: image.height, bitsPerComponent: 8, bytesPerRow: image.width * 4,
            space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
        context.draw(image, in: CGRect(x: 0, y: 0, width: image.width, height: image.height))
        let base = context.data!.assumingMemoryBound(to: UInt8.self)
        let width = image.width
        return PatternChecker(pattern).checkRGB { region in
            var sum = (0.0, 0.0, 0.0)
            for y in region.y..<(region.y + region.height) {
                for x in region.x..<(region.x + region.width) {
                    let at = (y * width + x) * 4
                    sum = (sum.0 + Double(base[at]), sum.1 + Double(base[at + 1]), sum.2 + Double(base[at + 2]))
                }
            }
            let count = Double(region.width * region.height)
            return (sum.0 / count, sum.1 / count, sum.2 / count)
        }
    }

    /// Mean and largest per-channel difference of two images of one size, drawn into sRGB RGBX.
    static func difference(_ a: CGImage, _ b: CGImage) -> (mean: Double, max: Int)? {
        guard a.width == b.width, a.height == b.height else { return nil }
        let rows = [a, b].map { image -> [UInt8] in
            var bytes = [UInt8](repeating: 0, count: image.width * image.height * 4)
            bytes.withUnsafeMutableBytes {
                let context = CGContext(
                    data: $0.baseAddress, width: image.width, height: image.height, bitsPerComponent: 8,
                    bytesPerRow: image.width * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                    bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
                context.draw(image, in: CGRect(x: 0, y: 0, width: image.width, height: image.height))
            }
            return bytes
        }
        var sum = 0
        var largest = 0
        for i in stride(from: 0, to: rows[0].count, by: 4) {
            for c in 0..<3 {
                let d = abs(Int(rows[0][i + c]) - Int(rows[1][i + c]))
                sum += d
                largest = max(largest, d)
            }
        }
        return (Double(sum) / Double(a.width * a.height * 3), largest)
    }

    static func patternImage(_ pattern: Pattern) -> CGImage {
        var bytes = [UInt8](repeating: 0, count: pattern.width * pattern.height * 4)
        bytes.withUnsafeMutableBytes {
            PatternWriter.writeRGB(
                pattern, into: .init(base: $0.baseAddress!, rowBytes: pattern.width * 4), channels: 4)
        }
        return CGImage(
            width: pattern.width, height: pattern.height, bitsPerComponent: 8, bitsPerPixel: 32,
            bytesPerRow: pattern.width * 4,
            space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
            provider: CGDataProvider(data: Data(bytes) as CFData)!, decode: nil, shouldInterpolate: false,
            intent: .defaultIntent)!
    }
}

/// One of the build's still images decoded through ImageIO (AppleJPEG/CMPhoto for JPEG and HEIC, HEVC for HEIC
/// tiles), timed, and checked against the pattern.
struct ImageDecodeTest: HWTest {
    let image: StillImage
    let subsystem = Subsystem.images
    var id: String { "image.\(image.format.rawValue)" }
    var name: String { "Decode \(image.file) (\(image.width)x\(image.height))" }

    func run(_ context: TestContext) async throws -> Outcome {
        let data = try Data(contentsOf: context.media.appendingPathComponent(image.file))
        guard let source = CGImageSourceCreateWithData(data as CFData, nil) else {
            return .fail("ImageIO cannot open it")
        }
        let options = [kCGImageSourceShouldCacheImmediately: true] as CFDictionary
        let jpeg = image.format.rawValue.hasPrefix("jpeg")
        let before = jpeg ? JPEGRoute.decodes() : nil
        let start = CFAbsoluteTimeGetCurrent()
        guard let decoded = CGImageSourceCreateImageAtIndex(source, 0, options) else {
            return .fail("ImageIO cannot decode it")
        }
        let milliseconds = (CFAbsoluteTimeGetCurrent() - start) * 1000
        let after = jpeg ? JPEGRoute.decodes() : nil
        let result = Pixels.check(decoded, pattern: image.pattern)
        var metrics = Metrics()
        metrics.set("decode_ms", milliseconds, digits: 1)
        metrics.set("mpix_s", Double(image.width * image.height) / milliseconds / 1000, digits: 1)
        metrics.set("error", result.lumaError, digits: 1)
        var checks = [
            ("\(image.width)x\(image.height) out", decoded.width == image.width && decoded.height == image.height),
            ("matches the pattern", result.matches(frame: 0, tolerance: image.tolerance)),
        ]
        if jpeg {
            checks += Self.againstSoftware(data, full: decoded, before: before, after: after, metrics: &metrics)
        }
        return .judged(checks, metrics, proof: CGImageSourceGetType(source).map { $0 as String } ?? "")
    }

    static let hardwareKey = "kCGImageSourceUseHardwareAcceleration" as CFString

    /// The route of the full decode, then a half-size thumbnail (CMPhoto's scaled decode writes a 4:4:4 file as '444f'
    /// and a 4:2:2 one as 'yuvf' under the interchange formats), each against ImageIO's software decode of the same
    /// request: NVJPG's formats the unit has not run before ('422f', '444f', 'yuvf') are proven or failed here.
    static func againstSoftware(
        _ data: Data, full: CGImage, before: Result<JPEGRoute.Counts, TestFailure>?,
        after: Result<JPEGRoute.Counts, TestFailure>?, metrics: inout Metrics
    ) -> [(String, Bool)] {
        guard let source = CGImageSourceCreateWithData(data as CFData, nil),
            let software = CGImageSourceCreateWithData(data as CFData, nil)
        else { return [("ImageIO opens it twice", false)] }
        let side = max(full.width, full.height) / 2
        let thumb: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true, kCGImageSourceThumbnailMaxPixelSize: side,
            kCGImageSourceShouldCacheImmediately: true,
        ]
        let half = CGImageSourceCreateThumbnailAtIndex(source, 0, thumb as CFDictionary)
        let afterThumb = JPEGRoute.decodes()
        var off = thumb
        off[hardwareKey] = false
        let softHalf = CGImageSourceCreateThumbnailAtIndex(software, 0, off as CFDictionary)
        let softFull = CGImageSourceCreateImageAtIndex(
            software, 0, [kCGImageSourceShouldCacheImmediately: true, hardwareKey: false] as CFDictionary)
        let afterSoftware = JPEGRoute.decodes()
        var checks: [(String, Bool)] = []
        switch (before, after, afterThumb, afterSoftware) {
        case (.success(let first)?, .success(let mid)?, .success(let last), .success(let soft)):
            metrics.set("route", mid.decodes > first.decodes ? "hardware" : "software")
            metrics.set("nvjpg_decodes", Int(last.decodes - first.decodes))
            metrics.set("thumb_route", last.decodes > mid.decodes ? "hardware" : "software")
            metrics.set("formats", JPEGRoute.names(last.formats))
            metrics.set("software_on_nvjpg", Int(soft.decodes - last.decodes))
            checks.append(("decoded on NVJPG", mid.decodes > first.decodes))
            checks.append(("thumbnail decoded on NVJPG", last.decodes > mid.decodes))
        case (.failure(let why)?, _, _, _), (_, .failure(let why)?, _, _), (_, _, .failure(let why), _),
            (_, _, _, .failure(let why)):
            metrics.set("route", "unknown")
            checks.append(("decoded on NVJPG (route unreadable: \(why.description))", false))
        default:
            break
        }
        for (label, hardware, reference) in [("full", full as CGImage?, softFull), ("thumb", half, softHalf)] {
            guard let hardware, let reference, let d = Pixels.difference(hardware, reference) else {
                checks.append(("\(label) decodes both ways at one size", false))
                continue
            }
            metrics.set("\(label)_vs_software_mean", d.mean, digits: 2)
            metrics.set("\(label)_vs_software_max", d.max)
            checks.append(("\(label) within 1.5 mean of the software decode", d.mean <= 1.5))
        }
        return checks
    }

    static var all: [ImageDecodeTest] { StillImage.all.map { ImageDecodeTest(image: $0) } }
}

/// A Metal-backed CIContext: a blur of a flat colour stays flat (exact), and a 1080p blur chain is timed.
struct CoreImageTest: HWTest {
    let id = "image.coreimage"
    let name = "Core Image filters on the GPU"
    let subsystem = Subsystem.images

    func run(_ context: TestContext) async throws -> Outcome {
        guard let device = MTLCreateSystemDefaultDevice() else { return .fail("no Metal device") }
        let ci = CIContext(mtlDevice: device, options: [.cacheIntermediates: false])
        let flat = CIImage(color: CIColor(red: 0.2, green: 0.4, blue: 0.6)).cropped(
            to: CGRect(x: 0, y: 0, width: 256, height: 256))
        let blurred = flat.applyingGaussianBlur(sigma: 6).cropped(to: CGRect(x: 64, y: 64, width: 128, height: 128))
        var pixel = [UInt8](repeating: 0, count: 4)
        ci.render(
            blurred.cropped(to: CGRect(x: 128, y: 128, width: 1, height: 1)), toBitmap: &pixel, rowBytes: 4,
            bounds: CGRect(x: 128, y: 128, width: 1, height: 1), format: .RGBA8,
            colorSpace: CGColorSpace(name: CGColorSpace.sRGB))
        let photo = CIImage(cgImage: Pixels.patternImage(Pattern(width: 1920, height: 1080, clip: 0x0C1)))
        let chain = photo.applyingGaussianBlur(sigma: 8).applyingFilter(
            "CIColorControls", parameters: ["inputSaturation": 1.4]
        )
        .cropped(to: photo.extent)
        guard
            let target = CGContext(
                data: nil, width: 1920, height: 1080, bitsPerComponent: 8, bytesPerRow: 1920 * 4,
                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return .fail("no bitmap") }
        let start = CFAbsoluteTimeGetCurrent()
        let runs = 10
        for _ in 0..<runs {
            ci.render(
                chain, toBitmap: target.data!, rowBytes: 1920 * 4, bounds: photo.extent, format: .RGBA8, colorSpace: nil
            )
        }
        let perRun = (CFAbsoluteTimeGetCurrent() - start) * 1000 / Double(runs)
        var metrics = Metrics()
        metrics.set("flat", pixel.prefix(3).map(String.init).joined(separator: ","))
        metrics.set("ms_1080p", perRun, digits: 1)
        return .judged(
            [
                (
                    "a blurred flat colour stays 51,102,153",
                    zip(pixel.prefix(3), [51, 102, 153]).allSatisfy { abs(Int($0) - $1) <= 1 }
                )
            ],
            metrics, proof: "CIContext(mtlDevice: \(device.name))")
    }
}
