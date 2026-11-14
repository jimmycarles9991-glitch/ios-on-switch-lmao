import Foundation

/// gpu.raw.xr_two_plane's CPU model of RGB10A8_2P_XR10 texels (the alpha byte above the code word, raw_xr.h): Metal's
/// values of their codes, source-over blending of premultiplied colours stored back as codes after each draw (the
/// storage and barrier forms fetch the stored codes), and bilinear samples.
enum XRTwoPlaneModel {
    /// A texel's red, green, blue ((c - 384) / 510) and alpha (byte / 255)
    static func values(_ t: UInt64) -> SIMD4<Double> {
        let channel = { (shift: UInt64) in Double(Int(t >> shift & 0x3ff) - 384) / 510 }
        return SIMD4(channel(20), channel(10), channel(0), Double(t >> 32 & 0xff) / 255)
    }

    /// raw_xr.h writeCode of each channel, as XRTwoPlaneTest.code but without rounding through a Float
    static func texel(_ v: SIMD4<Double>) -> UInt64 {
        let code = { (value: Double) -> UInt64 in
            let k = Int((min(abs(value), 1.5) * 510).rounded(.toNearestOrEven))
            return UInt64(min(max(value < 0 ? 384 - k : 384 + k, 0), 1023))
        }
        let alpha = UInt64((min(max(v.w, 0), 1) * 255).rounded(.toNearestOrEven))
        return alpha << 32 | code(v.x) << 20 | code(v.y) << 10 | code(v.z)
    }

    /// Source-over of a premultiplied colour (one, one minus source alpha on every channel) on a stored texel
    static func over(_ source: SIMD4<Double>, _ stored: UInt64) -> UInt64 {
        texel(source + values(stored) * (1 - source.w))
    }

    /// The largest difference between two texels' codes (the alpha byte as one code)
    static func codesApart(_ a: UInt64, _ b: UInt64) -> Int {
        let field = { (t: UInt64, shift: UInt64, mask: UInt64) in Int(t >> shift & mask) }
        let codes = [UInt64(0), 10, 20].map { abs(field(a, $0, 0x3ff) - field(b, $0, 0x3ff)) }
        return max(codes.max()!, abs(field(a, 32, 0xff) - field(b, 32, 0xff)))
    }

    /// Whether the pixel's centre lies in a rectangle of clip space (x right, y up), as an identity transform draws it
    static func covers(
        _ x: Int, _ y: Int, x0: Float, y0: Float, x1: Float, y1: Float, width: Int, height: Int
    ) -> Bool {
        let cx = (Float(x) + 0.5) / Float(width) * 2 - 1
        let cy = 1 - (Float(y) + 0.5) / Float(height) * 2
        return cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1
    }

    /// Texels beyond one code of the model, and those exactly one code off
    static func apart(_ got: [UInt64], _ want: [UInt64]) -> (wrong: Int, offByOne: Int) {
        var wrong = 0, offByOne = 0
        for (g, w) in zip(got, want) where g != w {
            if codesApart(g, w) > 1 { wrong += 1 } else { offByOne += 1 }
        }
        return (wrong, offByOne)
    }

    /// Each pixel's bilinear sample of `texels` (clamped to the edge) a third of a texel right of and below its own
    /// texel, its four channels' values, with sub-texel weights of `weight`: Metal's 1/3, or the 8-bit fraction a
    /// texture unit computes
    static func bilinear(_ texels: [UInt64], width: Int, height: Int, weight: Double) -> [SIMD4<Double>] {
        (0..<(width * height)).map { i in
            let (x, y) = (i % width, i / width)
            let (x1, y1) = (min(x + 1, width - 1), min(y + 1, height - 1))
            let top = values(texels[y * width + x]) * (1 - weight) + values(texels[y * width + x1]) * weight
            let bottom = values(texels[y1 * width + x]) * (1 - weight) + values(texels[y1 * width + x1]) * weight
            return top * (1 - weight) + bottom * weight
        }
    }

    /// Lanes of `halves` (RGBA16Float, four per pixel) that are not their texel's values as the nearest half (a nearest
    /// sample a third of a texel into the texel)
    static func nearestApart(_ halves: [UInt16], _ texels: [UInt64]) -> Int {
        var apart = 0
        for (pixel, t) in texels.enumerated() {
            let v = values(t)
            for c in 0..<4 where halves[pixel * 4 + c] != Float16(v[c]).bitPattern { apart += 1 }
        }
        return apart
    }

    /// Lanes of `halves` (RGBA16Float, four per pixel) outside the span of the two models by more than one step of
    /// their bits, and the largest such distance in steps
    static func linearApart(
        _ halves: [UInt16], exact: [SIMD4<Double>], fixed: [SIMD4<Double>]
    ) -> (apart: Int, steps: Int) {
        let order = { (h: UInt16) in h & 0x8000 != 0 ? -Int(h & 0x7fff) : Int(h) }
        var apart = 0, steps = 0
        for pixel in exact.indices {
            for c in 0..<4 {
                let a = order(Float16(exact[pixel][c]).bitPattern)
                let b = order(Float16(fixed[pixel][c]).bitPattern)
                let got = order(halves[pixel * 4 + c])
                let distance = max(min(a, b) - got, got - max(a, b), 0)
                if distance > 1 { apart += 1 }
                steps = max(steps, distance)
            }
        }
        return (apart, steps)
    }
}
