import Foundation

/// An icon's right and bottom edge bands against its left and top ones. IconServices masks every icon to a shape
/// symmetric about both centre lines, so the alpha of a pixel `k` from the right edge is the alpha `k` from the left
/// edge within antialiasing; a render whose last rows or columns go unwritten or keep another frame's texels (raw's
/// `edge miss` shape: one or two texels short of the right and bottom) breaks that symmetry there and nowhere else.
public enum IconEdgeBand {
    public static let band = 2
    public static let codes = 32

    /// Pixels of the right and bottom bands whose alpha differs from their mirror in the left and top ones by more than
    /// `codes`
    public struct Asymmetry: Equatable, Sendable {
        public var right = 0, bottom = 0

        public var field: String { "\(right)/\(bottom)" }
    }

    /// `alpha` by row of a `side`-pixel square icon
    public static func asymmetry(_ alpha: [UInt8], side: Int) -> Asymmetry {
        var out = Asymmetry()
        guard side > 2 * band, alpha.count >= side * side else { return out }
        for k in 0 ..< band {
            for i in 0 ..< side {
                let left = Int(alpha[i * side + k]), right = Int(alpha[i * side + side - 1 - k])
                let top = Int(alpha[k * side + i]), bottom = Int(alpha[(side - 1 - k) * side + i])
                if abs(right - left) > codes { out.right += 1 }
                if abs(bottom - top) > codes { out.bottom += 1 }
            }
        }
        return out
    }

    /// The alpha channel of premultiplied RGBA8 by row
    public static func alpha(_ rgba: [UInt8]) -> [UInt8] { stride(from: 3, to: rgba.count, by: 4).map { rgba[$0] } }
}
