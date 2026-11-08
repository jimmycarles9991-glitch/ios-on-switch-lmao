import Foundation

public struct Region: Equatable, Sendable {
    public let x: Int
    public let y: Int
    public let width: Int
    public let height: Int

    public init(x: Int, y: Int, width: Int, height: Int) {
        self.x = x
        self.y = y
        self.width = width
        self.height = height
    }
}

public struct FrameCheck: Equatable, Sendable {
    /// The frame number the barcode carries, when its CRC matches the clip; nil for an unreadable or foreign frame.
    public let frame: Int?
    public let code: UInt32
    /// Worst absolute error of a sample block's mean against the pattern, in 8-bit units.
    public let lumaError: Double
    public let chromaError: Double

    public func matches(frame expected: Int, tolerance: Double) -> Bool {
        frame == expected && lumaError <= tolerance && chromaError <= tolerance
    }
}

/// Reads a decoded picture back through mean-over-region samplers, so the same logic serves NV12, P010, BGRA and
/// RGBA images: the caller maps a region in picture coordinates to its own buffer.
public struct PatternChecker: Sendable {
    public let pattern: Pattern
    public static let gradientColumns = 8
    public static let gradientRows = 4
    public static let block = 8

    public init(_ pattern: Pattern) { self.pattern = pattern }

    public func cellRegion(row: Int, column: Int) -> Region {
        let (w, h) = (pattern.cellWidth, pattern.cellHeight)
        return Region(x: column * w + w / 4, y: row * h + h / 4, width: max(1, w / 2), height: max(1, h / 2))
    }

    public var gradientRegions: [Region] {
        let top = pattern.barcodeHeight
        let span = pattern.height - top
        var regions: [Region] = []
        for row in 0..<Self.gradientRows {
            for column in 0..<Self.gradientColumns {
                let x = (column * 2 + 1) * pattern.width / (Self.gradientColumns * 2) - Self.block / 2
                let y = top + (row * 2 + 1) * span / (Self.gradientRows * 2) - Self.block / 2
                regions.append(Region(x: x & ~1, y: y & ~1, width: Self.block, height: Self.block))
            }
        }
        return regions
    }

    public func readCode(brightness: (Region) -> Double) -> UInt32 {
        let threshold = Double(Pattern.black + Pattern.white) / 2
        var code: UInt32 = 0
        for index in 0..<(2 * Pattern.columns) {
            let region = cellRegion(row: index / Pattern.columns, column: index % Pattern.columns)
            code = code << 1 | (brightness(region) > threshold ? 1 : 0)
        }
        return code
    }

    public func frame(for code: UInt32) -> Int? {
        let number = Int(code >> 16)
        return pattern.code(frame: number) == code ? number : nil
    }

    func expectedLuma(_ region: Region, frame: Int) -> Double {
        mean(region) { Double(pattern.luma(x: $0, y: $1, frame: frame)) }
    }

    func expectedChroma(_ region: Region) -> (Double, Double) {
        (
            mean(region) { Double(pattern.chroma(x: $0, y: $1).cb) },
            mean(region) { Double(pattern.chroma(x: $0, y: $1).cr) }
        )
    }

    func mean(_ region: Region, _ value: (Int, Int) -> Double) -> Double {
        var sum = 0.0
        for y in region.y..<(region.y + region.height) {
            for x in region.x..<(region.x + region.width) { sum += value(x, y) }
        }
        return sum / Double(region.width * region.height)
    }

    /// Checks a YCbCr picture. `luma` and `chroma` return region means in 8-bit video-range units, `chroma` given the
    /// region in luma coordinates. The gradient is compared at the frame the barcode names (or `expected` when the
    /// barcode is unreadable), so a wrong frame shows up as a wrong number, not as a gradient error.
    public func check(
        expected: Int, luma: (Region) -> Double, chroma: ((Region) -> (cb: Double, cr: Double))?
    ) -> FrameCheck {
        let code = readCode(brightness: luma)
        let decoded = frame(for: code)
        let reference = decoded ?? expected
        var lumaError = 0.0
        var chromaError = 0.0
        for region in gradientRegions {
            lumaError = max(lumaError, abs(luma(region) - expectedLuma(region, frame: reference)))
            if let chroma {
                let (cb, cr) = chroma(region)
                let want = expectedChroma(region)
                chromaError = max(chromaError, abs(cb - want.0), abs(cr - want.1))
            }
        }
        return FrameCheck(frame: decoded, code: code, lumaError: lumaError, chromaError: chromaError)
    }

    /// Checks a full-range RGB picture (the still images): `rgb` returns region means in 0-255.
    public func checkRGB(expected: Int = 0, rgb: (Region) -> (r: Double, g: Double, b: Double)) -> FrameCheck {
        let code = readCode { region in
            let (r, g, b) = rgb(region)
            return 16 + (r + g + b) / 3 * 219 / 255
        }
        let decoded = frame(for: code)
        let reference = decoded ?? expected
        var error = 0.0
        for region in gradientRegions {
            let got = rgb(region)
            let want = (
                mean(region) { Double(pattern.rgb(x: $0, y: $1, frame: reference).r) },
                mean(region) { Double(pattern.rgb(x: $0, y: $1, frame: reference).g) },
                mean(region) { Double(pattern.rgb(x: $0, y: $1, frame: reference).b) }
            )
            error = max(error, abs(got.r - want.0), abs(got.g - want.1), abs(got.b - want.2))
        }
        return FrameCheck(frame: decoded, code: code, lumaError: error, chromaError: 0)
    }
}
