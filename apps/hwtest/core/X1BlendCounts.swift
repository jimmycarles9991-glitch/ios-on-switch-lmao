import Dispatch
import Foundation

/// gpu.x1.blend_exact's readback, one 4096 x 2056 BGRA8 draw a factor pair: pixel p is case p / 256 (a premultiplied
/// s <= sa, each the half nearest k / 255) over destination code d = p % 256, whose channels in memory order hold
/// (255 - d, 7d + 3, d, d). Each channel is held to every model: Metal's half arithmetic (separate, fma, exact on the
/// half inputs), the chip's blend, each product rounded alone, and the real result of the inputs.
extension X1Reference {
    public static let blendCases: [(UInt16, UInt16)] = (0 ..< 256).flatMap { a in
        (0 ... a).map { (UInt16($0), UInt16(a)) }
    }

    /// (blue, green, red, alpha) at pixel p.
    public static func blendDestination(_ p: Int) -> (Int, Int, Int, Int) {
        let d = p & 255
        return (255 - d, (d * 7 + 3) & 255, d, d)
    }

    /// Up and down count channels above and below Metal's separate code; `first` is the first channel off it.
    public struct BlendCounts: Sendable, Equatable {
        public var fma = 0, separate = 0, exact = 0, chip = 0, perProduct = 0, real = 0, up = 0, down = 0, maxDiff = 0
        public var first: String?

        public init() {}

        mutating func merge(_ other: BlendCounts) {
            fma += other.fma
            separate += other.separate
            exact += other.exact
            chip += other.chip
            perProduct += other.perProduct
            real += other.real
            up += other.up
            down += other.down
            maxDiff = max(maxDiff, other.maxDiff)
            first = first ?? other.first
        }
    }

    /// `chipAndSeparate` leaves the other models' counts 0.
    public enum BlendModels: Sendable {
        case all, chipAndSeparate
    }

    /// Every channel of every case against every model the caller reads, the cases split across the cores; each part
    /// counts its own run of pixels, in order, so the counts and the first miss are those of one pass from pixel 0.
    public static func blendCounts(
        _ bytes: UnsafeRawBufferPointer, source: Factor, destination: Factor, models: BlendModels = .all
    ) -> BlendCounts {
        let halves = (0 ..< 256).map { texel($0, top: 255) }
        let stores = halves.map { store($0) }
        var factors: [BlendFactors] = []
        factors.reserveCapacity(65536)
        for ai in 0 ..< 256 {
            let sa = halves[ai], sa8 = stores[ai]
            for d in 0 ..< 256 {
                factors.append(
                    BlendFactors(
                        fs: source.value(sa: sa, da: halves[d]), fd: destination.value(sa: sa, da: halves[d]),
                        fsExact: source.exact(sa: sa, da: d), fdExact: destination.exact(sa: sa, da: d),
                        f1: source.code(sa: sa8, da: d), f2: destination.code(sa: sa8, da: d)
                    )
                )
            }
        }
        let parts = 64
        var counts = [BlendCounts](repeating: BlendCounts(), count: parts)
        halves.withUnsafeBufferPointer { halves in
            stores.withUnsafeBufferPointer { stores in
                factors.withUnsafeBufferPointer { factors in
                    blendCases.withUnsafeBufferPointer { cases in
                        counts.withUnsafeMutableBufferPointer { out in
                            let tables = BlendTables(halves: halves, stores: stores, factors: factors, models: models)
                            DispatchQueue.concurrentPerform(iterations: parts) { part in
                                let first = cases.count * part / parts, last = cases.count * (part + 1) / parts
                                out[part] = tables.counts(bytes, cases, first ..< last)
                            }
                        }
                    }
                }
            }
        }
        return counts.reduce(into: BlendCounts()) { $0.merge($1) }
    }
}

/// A case's factors over one destination code: as values (Metal's: 1 - x rounded to half, the destination alpha as
/// its half texel), exact, and as codes (the chip's: the source alpha's stored code, the destination alpha's code).
struct BlendFactors {
    let fs: Double, fd: Double, fsExact: Double, fdExact: Double
    let f1: Int, f2: Int
}

struct BlendTables {
    let halves: UnsafeBufferPointer<Double>
    let stores: UnsafeBufferPointer<Int>
    let factors: UnsafeBufferPointer<BlendFactors>
    let models: X1Reference.BlendModels

    func counts(
        _ bytes: UnsafeRawBufferPointer, _ cases: UnsafeBufferPointer<(UInt16, UInt16)>, _ range: Range<Int>
    ) -> X1Reference.BlendCounts {
        var counts = X1Reference.BlendCounts()
        for c in range {
            let si = Int(cases[c].0), ai = Int(cases[c].1)
            for d in 0 ..< 256 {
                let p = c << 8 | d
                let f = factors[ai << 8 | d]
                let (b, g, r, a) = X1Reference.blendDestination(p)
                add(&counts, Int(bytes[p * 4]), si, ai, b, f, p: p, channel: 0)
                add(&counts, Int(bytes[p * 4 + 1]), si, ai, g, f, p: p, channel: 1)
                add(&counts, Int(bytes[p * 4 + 2]), si, ai, r, f, p: p, channel: 2)
                add(&counts, Int(bytes[p * 4 + 3]), ai, ai, a, f, p: p, channel: 3)
            }
        }
        return counts
    }

    @inline(__always)
    func add(
        _ counts: inout X1Reference.BlendCounts, _ have: Int, _ si: Int, _ ai: Int, _ dc: Int, _ f: BlendFactors,
        p: Int, channel: Int
    ) {
        let s = halves[si], texel = halves[dc]
        let want = X1Reference.unorm(X1Reference.halfSeparate(s, f.fs, texel, f.fd), top: 255)
        if have != want {
            counts.separate += 1
            if have > want { counts.up += 1 } else { counts.down += 1 }
            if counts.first == nil { counts.first = "\(p).\(channel):\(have),want_\(want)" }
        }
        counts.maxDiff = max(counts.maxDiff, abs(have - want))
        if have != X1Reference.chipBlend(stores[si], f.f1, dc, f.f2) { counts.chip += 1 }
        guard models == .all else { return }
        if have != X1Reference.unorm(X1Reference.halfFMA(s, f.fs, texel, f.fd), top: 255) { counts.fma += 1 }
        if have != X1Reference.unorm(X1Reference.exact(s, f.fs, texel, f.fd), top: 255) { counts.exact += 1 }
        if have != X1Reference.perProductBlend(stores[si], f.f1, dc, f.f2) { counts.perProduct += 1 }
        let real = s * f.fsExact + Double(dc) / 255 * f.fdExact
        if have != X1Reference.unorm(real, top: 255) { counts.real += 1 }
    }
}
