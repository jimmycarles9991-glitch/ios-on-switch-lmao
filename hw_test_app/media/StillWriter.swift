import CoreGraphics
import Foundation
import HWTestCore
import ImageIO
import SwitchTurboJPEG
import UniformTypeIdentifiers

/// The still images: JPEGs from libjpeg-turbo (ImageIO cannot choose the chroma subsampling), PNG and HEIC from
/// ImageIO.
enum StillWriter {
    struct Failure: Error, CustomStringConvertible {
        let description: String
    }

    static func write(_ image: StillImage, to url: URL) throws {
        let pattern = image.pattern
        var pixels = [UInt8](repeating: 0, count: pattern.width * pattern.height * 3)
        pixels.withUnsafeMutableBytes {
            PatternWriter.writeRGB(
                pattern, into: .init(base: $0.baseAddress!, rowBytes: pattern.width * 3), channels: 3)
        }
        switch image.format {
        case .jpeg420: try writeJPEG(pixels, pattern, subsampling: TJSAMP_420, to: url)
        case .jpeg422: try writeJPEG(pixels, pattern, subsampling: TJSAMP_422, to: url)
        case .jpeg444: try writeJPEG(pixels, pattern, subsampling: TJSAMP_444, to: url)
        case .png: try writeImageIO(pixels, pattern, type: .png, to: url)
        case .heic, .heicGrid:
            try writeImageIO(pixels, pattern, type: .heic, to: url)
            let grid = try Data(contentsOf: url).range(of: Data("grid".utf8)) != nil
            guard grid == (image.format == .heicGrid) else {
                throw Failure(description: "\(image.file): ImageIO wrote \(grid ? "a grid" : "no grid")")
            }
        }
    }

    static func writeJPEG(_ pixels: [UInt8], _ pattern: Pattern, subsampling: TJSAMP, to url: URL) throws {
        guard let handle = switch_tj3_compressor() else { throw Failure(description: "tj3Init failed") }
        defer { tj3Destroy(handle) }
        tj3Set(handle, Int32(TJPARAM_SUBSAMP.rawValue), Int32(subsampling.rawValue))
        tj3Set(handle, Int32(TJPARAM_QUALITY.rawValue), 92)
        var jpeg: UnsafeMutablePointer<UInt8>?
        var size = 0
        let status = tj3Compress8(
            handle, pixels, Int32(pattern.width), 0, Int32(pattern.height), Int32(TJPF_RGB.rawValue), &jpeg, &size)
        defer { tj3Free(jpeg) }
        guard status == 0, let jpeg else {
            throw Failure(description: "tj3Compress8: \(String(cString: tj3GetErrorStr(handle)))")
        }
        try Data(bytes: jpeg, count: size).write(to: url)
    }

    static func writeImageIO(_ pixels: [UInt8], _ pattern: Pattern, type: UTType, to url: URL) throws {
        let provider = CGDataProvider(data: Data(pixels) as CFData)!
        guard
            let image = CGImage(
                width: pattern.width, height: pattern.height, bitsPerComponent: 8, bitsPerPixel: 24,
                bytesPerRow: pattern.width * 3, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue), provider: provider, decode: nil,
                shouldInterpolate: false, intent: .defaultIntent),
            let destination = CGImageDestinationCreateWithURL(url as CFURL, type.identifier as CFString, 1, nil)
        else { throw Failure(description: "\(url.lastPathComponent): cannot make the image") }
        CGImageDestinationAddImage(
            destination, image, [kCGImageDestinationLossyCompressionQuality: 0.9] as CFDictionary)
        guard CGImageDestinationFinalize(destination) else {
            throw Failure(description: "\(url.lastPathComponent): ImageIO could not write it")
        }
    }
}
