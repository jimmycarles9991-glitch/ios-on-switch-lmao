import CoreGraphics
import CoreImage
import Foundation
import HWTestCore
import ImageIO
import Metal
import UniformTypeIdentifiers

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
        let start = CFAbsoluteTimeGetCurrent()
        guard let decoded = CGImageSourceCreateImageAtIndex(source, 0, options) else {
            return .fail("ImageIO cannot decode it")
        }
        let milliseconds = (CFAbsoluteTimeGetCurrent() - start) * 1000
        let result = Pixels.check(decoded, pattern: image.pattern)
        var metrics = Metrics()
        metrics.set("ms", milliseconds, digits: 1)
        metrics.set("mpix_s", Double(image.width * image.height) / milliseconds / 1000, digits: 1)
        metrics.set("error", result.lumaError, digits: 1)
        return .judged(
            [
                ("\(image.width)x\(image.height) out", decoded.width == image.width && decoded.height == image.height),
                ("matches the pattern", result.matches(frame: 0, tolerance: image.tolerance)),
            ], metrics, proof: CGImageSourceGetType(source).map { $0 as String } ?? "")
    }

    static var all: [ImageDecodeTest] { StillImage.all.map { ImageDecodeTest(image: $0) } }
}

/// HEIC written on the device (ImageIO over the HEVC encoder) and read back.
struct HEICEncodeTest: HWTest {
    let id = "image.heic_encode"
    let name = "HEIC encode and decode on the device"
    let subsystem = Subsystem.images

    func run(_ context: TestContext) async throws -> Outcome {
        let pattern = Pattern(width: 1024, height: 768, clip: 0x5E1C)
        let data = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(data, UTType.heic.identifier as CFString, 1, nil)
        else {
            return .fail("ImageIO offers no HEIC destination")
        }
        let start = CFAbsoluteTimeGetCurrent()
        CGImageDestinationAddImage(
            destination, Pixels.patternImage(pattern), [kCGImageDestinationLossyCompressionQuality: 0.9] as CFDictionary
        )
        guard CGImageDestinationFinalize(destination) else { return .fail("CGImageDestinationFinalize failed") }
        let encodeMs = (CFAbsoluteTimeGetCurrent() - start) * 1000
        guard let source = CGImageSourceCreateWithData(data as CFData, nil),
            let decoded = CGImageSourceCreateImageAtIndex(source, 0, nil)
        else { return .fail("the HEIC written does not decode") }
        let result = Pixels.check(decoded, pattern: pattern)
        var metrics = Metrics()
        metrics.set("encode_ms", encodeMs, digits: 1)
        metrics.set("bytes", data.length)
        metrics.set("error", result.lumaError, digits: 1)
        return .judged([("decodes back to the pattern", result.matches(frame: 0, tolerance: 12))], metrics)
    }
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
