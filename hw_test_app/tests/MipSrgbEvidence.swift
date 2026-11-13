import Foundation

/// What an sRGB chain's proven steps followed: Metal's linear-light step (MipReference) or the step of the codes taken as
/// unorm, and the first field more than one code from Metal's with the codes it came from. Both GM20B routes (the 2D
/// engine and the texture unit's filter under a 3D pass) read the source through the texture unit's sRGB decode, so the
/// two readings separate a filter of codes (most fields many codes off Metal's, within one of the codes') from a
/// precision loss in linear light (a few fields two codes off Metal's).
extension MipChains {
    var codesLayout: MipLayout {
        MipLayout(
            bytes: layout.bytes,
            fields: layout.fields.map { MipLayout.Field(shift: $0.shift, width: $0.width, kind: .unorm) })
    }

    private func provenSteps(_ levels: [[UInt8]], _ w: Int, _ h: Int) -> [(Int, (Int, Int), (Int, Int))] {
        let dims = Self.dims(w, h)
        return (1..<min(dims.count, levels.count)).compactMap { l in
            let (s, d) = (dims[l - 1], dims[l])
            return Self.proven(s.0, d.0) && Self.proven(s.1, d.1) ? (l, s, d) : nil
        }
    }

    /// The proven steps of `levels` against the step of the level above's codes taken as unorm
    func compareCodes(_ levels: [[UInt8]], _ w: Int, _ h: Int, into tally: inout MipTally) {
        for (l, (sw, sh), (dw, dh)) in provenSteps(levels, w, h) {
            tally.add(
                levels[l], MipReference.step(levels[l - 1], sw, sh, dw, dh, codesLayout), texels: dw * dh, codesLayout)
        }
    }

    /// The first field of a proven step more than one code from Metal's:
    /// "<from>><to>@<x>,<y>c<field>:<got>/<want>/<codes' step>:<the 2x2 codes above>", nil when there is none
    func firstBeyondOne(_ levels: [[UInt8]], _ w: Int, _ h: Int) -> String? {
        for (l, (sw, sh), (dw, dh)) in provenSteps(levels, w, h) {
            let want = MipReference.step(levels[l - 1], sw, sh, dw, dh, layout)
            for t in 0..<dw * dh {
                for (i, f) in layout.fields.enumerated() {
                    let got = layout.code(levels[l], texel: t, field: f), metal = layout.code(want, texel: t, field: f)
                    guard (got > metal ? got - metal : metal - got) > 1 else { continue }
                    let (x, y) = (t % dw, t / dw)
                    let codes = MipReference.step(levels[l - 1], sw, sh, dw, dh, codesLayout)
                    let above = [(0, 0), (1, 0), (0, 1), (1, 1)].map { ox, oy in
                        layout.code(
                            levels[l - 1], texel: min(2 * y + oy, sh - 1) * sw + min(2 * x + ox, sw - 1), field: f)
                    }
                    return
                        "\(sw)x\(sh)>\(dw)x\(dh)@\(x),\(y)c\(i):\(got)/\(metal)/\(layout.code(codes, texel: t, field: f)):"
                        + above.map(String.init).joined(separator: "_")
                }
            }
        }
        return nil
    }
}

/// One sRGB format's evidence on the line: each tally as <fields beyond one>/<largest difference>
struct MipSrgbEvidence {
    var twod = MipTally(), threeD = MipTally(), level1 = MipTally(), codes = MipTally()
    var first2d: String?, first3d: String?

    func cell(_ name: String) -> String {
        let pair = { (t: MipTally) in "\(t.wrong)/\(t.maxDiff)" }
        return "\(name):2d=\(pair(twod)):3d=\(pair(threeD)):l1=\(pair(level1)):codes=\(pair(codes))"
    }
}
