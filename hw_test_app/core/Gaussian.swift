import Foundation

/// A Gaussian blur computed on the CPU in double precision: the reference for MPS's and Core Image's.
public enum Gaussian {
    /// Normalised weights from -radius to +radius; radius ceil(3 sigma) unless given.
    public static func weights(sigma: Double, radius: Int? = nil) -> [Double] {
        let r = radius ?? Int((3 * sigma).rounded(.up))
        let raw = (-r...r).map { exp(-Double($0 * $0) / (2 * sigma * sigma)) }
        let sum = raw.reduce(0, +)
        return raw.map { $0 / sum }
    }

    /// Separable, edges clamped (each tap past the edge reads the edge texel).
    public static func blur(_ image: Raster, sigma: Double, radius: Int? = nil) -> Raster {
        let w = weights(sigma: sigma, radius: radius)
        let r = w.count / 2
        let horizontal = pass(image, w, r) { x, y, dx in (min(max(x + dx, 0), image.width - 1), y) }
        return pass(horizontal, w, r) { x, y, dy in (x, min(max(y + dy, 0), image.height - 1)) }
    }

    static func pass(
        _ image: Raster, _ weights: [Double], _ r: Int, _ tap: (Int, Int, Int) -> (Int, Int)
    ) -> Raster {
        Raster(width: image.width, height: image.height, channels: image.channels, levels: image.levels) { x, y, c in
            var sum = 0.0
            for (i, weight) in weights.enumerated() {
                let (tx, ty) = tap(x, y, i - r)
                sum += weight * Double(image[tx, ty, c])
            }
            return Float(sum)
        }
    }
}
