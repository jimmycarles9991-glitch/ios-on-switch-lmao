import Foundation

/// A 'v0a8' picture's codes (BT.709 video-range Y in plane 0, CbCr in plane 1, full-range alpha in plane 2) and what
/// a compositor puts on screen when it draws one over a solid background. Each Model is one way of reading and
/// blending the planes, the right one or a known mistake, so a measured composite can name the plane or blend that
/// went wrong.
public enum VideoAlpha {
    public struct Codes: Equatable, Sendable {
        public let y: UInt8
        public let cb: UInt8
        public let cr: UInt8
        public let a: UInt8

        public init(y: UInt8, cb: UInt8, cr: UInt8, a: UInt8) {
            self.y = y
            self.cb = cb
            self.cr = cr
            self.a = a
        }
    }

    /// straight: C·α + B·(1-α); premultiplied: C + B·(1-α); squared: C·α² + B·(1-α), straight colour
    /// multiplied by alpha once more than its mode says.
    public enum Mode: String, CaseIterable, Sendable {
        case straight, premultiplied, squared
    }

    /// Where the blend's alpha comes from: plane 2, plane 0 or plane 1's Cb read in its place, or no alpha at all.
    public enum AlphaSource: String, CaseIterable, Sendable {
        case plane2, luma, cb, one, zero
    }

    public enum Range: String, CaseIterable, Sendable {
        case video, full
    }

    /// The blend done on sRGB-encoded values, or in linear light.
    public enum Space: String, CaseIterable, Sendable {
        case gamma, linear
    }

    public struct Model: Hashable, Sendable {
        public let mode: Mode
        public let alpha: AlphaSource
        public let range: Range
        public let space: Space

        public init(_ mode: Mode, _ alpha: AlphaSource = .plane2, _ range: Range = .video, _ space: Space = .gamma) {
            self.mode = mode
            self.alpha = alpha
            self.range = range
            self.space = space
        }

        public var name: String { "\(mode.rawValue)/\(alpha.rawValue)/\(range.rawValue)/\(space.rawValue)" }

        public static let all: [Model] = Mode.allCases.flatMap { mode in
            AlphaSource.allCases.flatMap { alpha in
                Range.allCases.flatMap { range in Space.allCases.map { Model(mode, alpha, range, $0) } }
            }
        }
    }

    /// The HW Test's patches (ui.video_alpha): two greys, then three colours, each at every alpha.
    public static let colours: [SIMD3<Float>] = [
        SIMD3(0.5, 0.5, 0.5), SIMD3(1, 1, 1), SIMD3(0.75, 0.25, 0.2), SIMD3(0.2, 0.7, 0.3), SIMD3(0.25, 0.3, 0.8),
    ]
    public static let greys = 2
    public static let alphas: [UInt8] = [255, 192, 128, 64, 0]
    public static let background = SIMD3<Float>(0.1, 0.45, 0.65)

    /// Every patch's codes by colour, then by alpha; a premultiplied picture holds each colour times its alpha.
    public static func grid(_ mode: Mode) -> [[Codes]] {
        colours.map { colour in
            alphas.map { a in encode(mode == .premultiplied ? colour * (Float(a) / 255) : colour, alpha: a) }
        }
    }

    static let kr: Float = 0.2126
    static let kb: Float = 0.0722
    static let kg: Float = 1 - kr - kb

    static func code(_ v: Float) -> UInt8 { UInt8(min(max(v.rounded(), 0), 255)) }

    /// The codes of an sRGB-encoded colour, BT.709 video range.
    public static func encode(_ rgb: SIMD3<Float>, alpha: UInt8) -> Codes {
        let luma = kr * rgb.x + kg * rgb.y + kb * rgb.z
        let pb = (rgb.z - luma) / (2 * (1 - kb))
        let pr = (rgb.x - luma) / (2 * (1 - kr))
        return Codes(y: code(16 + 219 * luma), cb: code(128 + 224 * pb), cr: code(128 + 224 * pr), a: alpha)
    }

    /// The colour the codes hold, read with the BT.709 matrix at `range`.
    public static func decode(_ c: Codes, range: Range) -> SIMD3<Float> {
        let (luma, pb, pr): (Float, Float, Float) =
            range == .video
            ? ((Float(c.y) - 16) / 219, (Float(c.cb) - 128) / 224, (Float(c.cr) - 128) / 224)
            : (Float(c.y) / 255, (Float(c.cb) - 128) / 255, (Float(c.cr) - 128) / 255)
        let r = luma + 2 * (1 - kr) * pr
        let b = luma + 2 * (1 - kb) * pb
        let g = (luma - kr * r - kb * b) / kg
        return SIMD3(r, g, b).clamped(lowerBound: SIMD3(repeating: 0), upperBound: SIMD3(repeating: 1))
    }

    public static func toLinear(_ v: Float) -> Float {
        v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4)
    }

    public static func toGamma(_ v: Float) -> Float {
        v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055
    }

    static func alpha(_ c: Codes, _ source: AlphaSource) -> Float {
        switch source {
        case .plane2: return Float(c.a) / 255
        case .luma: return Float(c.y) / 255
        case .cb: return Float(c.cb) / 255
        case .one: return 1
        case .zero: return 0
        }
    }

    /// The sRGB-encoded colour on screen where `c` is drawn over `background` the way `model` reads and blends it.
    public static func composite(_ c: Codes, over background: SIMD3<Float>, _ model: Model) -> SIMD3<Float> {
        let a = alpha(c, model.alpha)
        let linear = model.space == .linear
        let source = decode(c, range: model.range)
        let (s, b) = linear ? (source.map(toLinear), background.map(toLinear)) : (source, background)
        let colour: SIMD3<Float>
        switch model.mode {
        case .straight: colour = s * a
        case .premultiplied: colour = s
        case .squared: colour = s * a * a
        }
        let out = (colour + b * (1 - a)).clamped(lowerBound: SIMD3(repeating: 0), upperBound: SIMD3(repeating: 1))
        return linear ? out.map(toGamma) : out
    }
}

extension SIMD3 where Scalar == Float {
    func map(_ f: (Float) -> Float) -> SIMD3<Float> { SIMD3(f(x), f(y), f(z)) }
}
