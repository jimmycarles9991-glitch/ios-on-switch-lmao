import Foundation

/// Which explanation of the scroll edge keyline's purple row (ui.pocket_keyline) its numbers support, as claim_verdict=:
/// fill, UIKit filled the keyline layer with opaque magenta, the colour it gives a value it could not resolve: the row
/// is that fill drawn as asked, so no pixel reasoning applies (a magenta texel is the fill, not an unwritten one);
/// a, the keyline state's own draw is wrong (its offscreen replica drew another value into an attachment);
/// b, the draw is right and the row is damaged or left unwritten after it (the replica is exact while the composited
/// row is purple; a row holding Metal's fresh texture texel exactly is a row nothing wrote);
/// c, the blend of the keyline's translucent white rounds a channel differently (the row is not purple and within
/// `rounding` codes of the source-over value over the row without the keyline);
/// d, the render server's picture and its framebuffer are clean (what the panel shows comes after the framebuffer);
/// a_or_b, the row is purple but the replica did not run; none, nothing is wrong in this run.
public enum KeylineClaim {
    public struct Evidence: Sendable {
        /// The composited picture's keyline row, its colour with the keyline hidden, and the framebuffer's at the
        /// same place (nil: no read-back)
        public var line: [Int]
        public var backdrop: [Int]?
        public var framebuffer: [Int]?
        /// What the replica drew wrong ("c0.g"), "" when it drew every attachment as expected, nil when it did not run
        public var replicaWrong: String?
        public var alpha: Double
        /// The keyline layer's own fill, R, G, B, A (nil: unknown)
        public var fill: [Double]?

        public init(
            line: [Int], backdrop: [Int]?, framebuffer: [Int]?, replicaWrong: String?, alpha: Double,
            fill: [Double]? = nil
        ) {
            self.line = line
            self.backdrop = backdrop
            self.framebuffer = framebuffer
            self.replicaWrong = replicaWrong
            self.alpha = alpha
            self.fill = fill
        }
    }

    public static func magenta(_ rgba: [Double]) -> Bool {
        rgba.count == 4 && zip(rgba, [1.0, 0, 1, 1]).allSatisfy { abs($0 - $1) < 1e-6 }
    }

    /// The keyline layer's fill against those UIKit gives the device class (`wants`): "want", "magenta" or "other"
    public static func fillVerdict(_ fill: [Double]?, wants: [[Double]]) -> String {
        guard let fill else { return "other" }
        if magenta(fill) { return "magenta" }
        let same = { (want: [Double]) in
            fill.count == want.count && zip(fill, want).allSatisfy { abs($0 - $1) < 0.001 }
        }
        return wants.contains(where: same) ? "want" : "other"
    }

    public static let rounding = 2

    public static func purple(_ rgb: [Int]) -> Bool {
        rgb.count >= 3 && rgb[0] - rgb[1] > 40 && rgb[2] - rgb[1] > 40
    }

    /// Metal's fresh texture texel of a BGRA8 or RGBA8 texture (opaque magenta), as a picture's sRGB conversion may
    /// leave it
    public static func fresh(_ rgb: [Int]) -> Bool {
        rgb.count >= 3 && rgb[0] >= 254 && rgb[1] <= 1 && rgb[2] >= 254
    }

    /// The keyline's colour (white unless `rgb`) premultiplied as Core Animation's vertex carries it, in halves, over
    /// `backdrop`, source-over in unorm8
    public static func expected(over backdrop: [Int], alpha: Double, rgb: [Double] = [1, 1, 1]) -> [Int] {
        let a = half(alpha)
        return zip(backdrop.prefix(3), rgb).map { b, c in
            Int(((half(c * alpha) + Double(b) / 255 * (1 - a)) * 255).rounded())
        }
    }

    /// `x` rounded to a half's 11 significant bits (normal halves)
    public static func half(_ x: Double) -> Double {
        guard x != 0 else { return 0 }
        let step = pow(2, (log2(abs(x))).rounded(.down) - 10)
        return (x / step).rounded(.toNearestOrEven) * step
    }

    public static func verdict(_ e: Evidence) -> (claim: String, why: String) {
        if let fill = e.fill, magenta(fill) {
            return ("fill", "UIKit filled the keyline layer opaque magenta: the row is that fill as drawn")
        }
        let framebufferPurple = e.framebuffer.map(purple) ?? false
        if !purple(e.line) && !framebufferPurple {
            if let backdrop = e.backdrop {
                let want = expected(over: backdrop, alpha: e.alpha)
                let off = zip(e.line, want).map { abs($0 - $1) }
                if let worst = off.max(), worst > 0, worst <= rounding {
                    return ("c", "a channel rounds off the source-over value by \(worst)")
                }
            }
            if e.framebuffer != nil { return ("d", "the render and its framebuffer are clean at the keyline") }
            return ("none", "no purple keyline row in this run")
        }
        if let wrong = e.replicaWrong, !wrong.isEmpty { return ("a", "the keyline state's replica drew \(wrong)") }
        guard e.replicaWrong != nil else { return ("a_or_b", "a purple row and no replica") }
        if purple(e.line) && fresh(e.line) {
            return ("b", "the row holds the fresh texture texel while the keyline's draw is exact: nothing wrote it")
        }
        if !purple(e.line) { return ("b", "the snapshot is clean and the framebuffer's row purple") }
        return ("b", "the keyline's draw is exact and its row is purple: damaged after the draw")
    }
}
