import CoreGraphics
import Foundation
import HWTestCore
import ImageIO

/// Rasters as PNGs a person can open: values clamped to [0, 1], one channel shown grey, two as red and green, a
/// missing alpha opaque. The diff image is the reference dimmed with every wrong pixel magenta.
extension Raster {
    public func rgba8() -> [UInt8] {
        var out = [UInt8](repeating: 255, count: width * height * 4)
        let byte = { (v: Float) -> UInt8 in v.isNaN ? 0 : UInt8((Swift.min(Swift.max(v, 0), 1) * 255).rounded()) }
        for i in 0..<(width * height) {
            let p = Array(samples[(i * channels)..<((i + 1) * channels)])
            switch channels {
            case 1: out[i * 4] = byte(p[0]); out[i * 4 + 1] = byte(p[0]); out[i * 4 + 2] = byte(p[0])
            case 2: out[i * 4] = byte(p[0]); out[i * 4 + 1] = byte(p[1]); out[i * 4 + 2] = 0
            default:
                for c in 0..<3 { out[i * 4 + c] = byte(p[c]) }
                if channels == 4 { out[i * 4 + 3] = byte(p[3]) }
            }
        }
        return out
    }

    public func cgImage() -> CGImage? {
        Self.image(rgba8(), width: width, height: height)
    }

    public func png() -> Data? { cgImage().flatMap(Self.png) }

    static func image(_ bytes: [UInt8], width: Int, height: Int) -> CGImage? {
        guard let provider = CGDataProvider(data: Data(bytes) as CFData) else { return nil }
        return CGImage(
            width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
            space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue), provider: provider,
            decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }

    static func png(_ image: CGImage) -> Data? {
        let data = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(data, "public.png" as CFString, 1, nil) else {
            return nil
        }
        CGImageDestinationAddImage(destination, image, nil)
        return CGImageDestinationFinalize(destination) ? data as Data : nil
    }
}

extension SceneCheck {
    public func diffPNG() -> Data? {
        var bytes = want.rgba8()
        for i in 0..<(want.width * want.height) {
            if diff.mask[i] {
                bytes.replaceSubrange((i * 4)..<(i * 4 + 4), with: [255, 0, 255, 255])
            } else {
                for c in 0..<3 { bytes[i * 4 + c] /= 3 }
                bytes[i * 4 + 3] = 255
            }
        }
        return Raster.image(bytes, width: want.width, height: want.height).flatMap(Raster.png)
    }

    /// got, want and diff PNGs into `directory`, named after `stem`.
    public func save(to directory: URL, stem: String) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        for (suffix, data) in [("got", got.png()), ("want", want.png()), ("diff", diffPNG())] {
            try data?.write(to: directory.appendingPathComponent("\(stem)-\(suffix).png"), options: .atomic)
        }
    }
}
