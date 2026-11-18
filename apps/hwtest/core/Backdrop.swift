import Foundation

/// What a blurred backdrop must look like, read off the composited picture alone (Core Animation's CPU renderer
/// draws no backdrop blur, so there is no picture to compare with).
public enum Backdrop {
    public struct Stripes: Equatable, Sendable {
        /// The most any column changes down its length: the backdrop does not change along y.
        public let columnSpread: Float
        /// The largest step between neighbouring columns: a blur leaves no sharp edge.
        public let step: Float
        /// The largest difference along one row: what is left of the stripes' contrast.
        public let contrast: Float
    }

    /// `image` lies over vertical stripes (a backdrop that varies across x only).
    public static func overStripes(_ image: Raster) -> Stripes {
        var (spread, step, contrast) = (Float(0), Float(0), Float(0))
        for c in 0..<min(3, image.channels) {
            for x in 0..<image.width {
                var (low, high) = (Float.infinity, -Float.infinity)
                for y in 0..<image.height {
                    low = min(low, image[x, y, c])
                    high = max(high, image[x, y, c])
                    if x > 0 { step = max(step, abs(image[x, y, c] - image[x - 1, y, c])) }
                }
                spread = max(spread, high - low)
            }
            let row = (0..<image.width).map { image[$0, image.height / 2, c] }
            contrast = max(contrast, (row.max() ?? 0) - (row.min() ?? 0))
        }
        return Stripes(columnSpread: spread, step: step, contrast: contrast)
    }

    /// The most any channel varies over `image`, and its mean colour.
    public static func overUniform(_ image: Raster) -> (spread: Float, mean: [Float]) {
        var spread: Float = 0
        var mean: [Float] = []
        for c in 0..<image.channels {
            let values = (0..<(image.width * image.height)).map { image.samples[$0 * image.channels + c] }
            spread = max(spread, (values.max() ?? 0) - (values.min() ?? 0))
            mean.append(values.reduce(0, +) / Float(max(1, values.count)))
        }
        return (spread, mean)
    }
}
