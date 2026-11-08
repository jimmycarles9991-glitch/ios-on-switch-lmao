import Foundation

/// A picture as floats, one per channel, rows packed: what a GPU scene read back or what its reference says it should
/// hold. `levels` is what one unit is shown as in the log (255 for 8-bit unorm, 1 for float formats).
public struct Raster: Equatable, Sendable {
    public let width: Int
    public let height: Int
    public let channels: Int
    public let levels: Float
    public var samples: [Float]

    public init(width: Int, height: Int, channels: Int, levels: Float = 255, samples: [Float]) {
        precondition(samples.count == width * height * channels, "raster \(width)x\(height)x\(channels) size")
        self.width = width
        self.height = height
        self.channels = channels
        self.levels = levels
        self.samples = samples
    }

    public init(width: Int, height: Int, channels: Int, levels: Float = 255, fill: (Int, Int, Int) -> Float) {
        var samples = [Float](repeating: 0, count: width * height * channels)
        for y in 0..<height {
            for x in 0..<width {
                for c in 0..<channels { samples[(y * width + x) * channels + c] = fill(x, y, c) }
            }
        }
        self.init(width: width, height: height, channels: channels, levels: levels, samples: samples)
    }

    /// 8-bit unorm bytes, rows packed.
    public init(unorm8 bytes: [UInt8], width: Int, height: Int, channels: Int) {
        self.init(width: width, height: height, channels: channels, samples: bytes.map { Float($0) / 255 })
    }

    public subscript(x: Int, y: Int, c: Int) -> Float {
        get { samples[(y * width + x) * channels + c] }
        set { samples[(y * width + x) * channels + c] = newValue }
    }

    public func pixel(_ x: Int, _ y: Int) -> [Float] {
        let at = (y * width + x) * channels
        return Array(samples[at..<(at + channels)])
    }

    public func cropped(x: Int, y: Int, width: Int, height: Int) -> Raster {
        Raster(width: width, height: height, channels: channels, levels: levels) { px, py, c in
            self[x + px, y + py, c]
        }
    }

    /// Each `factor` x `factor` block averaged into one pixel (a partial block at the edge is dropped): anti-aliasing
    /// that differs between two renderers averages out, blocks of noise and streaks do not.
    public func boxDownsampled(by factor: Int) -> Raster {
        let (w, h) = (width / factor, height / factor)
        let area = Float(factor * factor)
        return Raster(width: w, height: h, channels: channels, levels: levels) { x, y, c in
            var sum: Float = 0
            for dy in 0..<factor {
                for dx in 0..<factor { sum += self[x * factor + dx, y * factor + dy, c] }
            }
            return sum / area
        }
    }

    /// The value as the log shows it: whole levels for unorm formats, three decimals for float ones.
    public func shown(_ value: Float) -> String {
        guard value.isFinite, levels != 1 else { return MetricValue.format(Double(value), digits: 3) }
        return String(Int((value * levels).rounded()))
    }

    public func shown(_ pixel: [Float]) -> String { pixel.map(shown).joined(separator: ",") }
}
