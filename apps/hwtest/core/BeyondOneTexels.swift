import Foundation

/// A resolve's texels more than one code off Metal's rule in some channel, for the report: the first `shown` with their
/// codes, Metal's and each sample's, and how many in all. `text` is
/// `<count>|<x>,<y>:<got>~<Metal's>@<sample 0>/<sample 1>/...|...`, each a texel's RGBA codes in hex joined by dots.
public struct BeyondOneTexels: Sendable, Equatable {
    public let shown: Int
    public private(set) var count = 0
    public private(set) var entries: [String] = []

    public init(shown: Int = 8) { self.shown = shown }

    public static func beyondOne(_ got: [UInt32], _ want: [UInt32]) -> Bool {
        zip(got, want).contains { abs(Int64($0) - Int64($1)) > 1 }
    }

    public mutating func add(x: Int, y: Int, got: [UInt32], want: [UInt32], samples: [[UInt32]]) {
        guard Self.beyondOne(got, want) else { return }
        count += 1
        guard entries.count < shown else { return }
        let codes = { (c: [UInt32]) in c.map { String($0, radix: 16) }.joined(separator: ".") }
        entries.append("\(x),\(y):\(codes(got))~\(codes(want))@" + samples.map(codes).joined(separator: "/"))
    }

    public var text: String { ([String(count)] + entries).joined(separator: "|") }

    /// gpu.gm20b.resolve_twod's BGRA8 scene: `got` against `want`, both BGRA8 rows `width` wide, each texel's samples
    /// ResolveRef.sampleHash's bytes (r, g, b, a)
    public static func bgra8(_ got: [UInt8], _ want: [UInt8], width: Int, samples: Int, shown: Int = 8)
        -> BeyondOneTexels
    {
        var out = BeyondOneTexels(shown: shown)
        let rgba = { (b: [UInt8], t: Int) in [b[4 * t + 2], b[4 * t + 1], b[4 * t], b[4 * t + 3]].map { UInt32($0) } }
        for t in 0 ..< min(got.count, want.count) / 4 {
            let g = rgba(got, t), w = rgba(want, t)
            guard beyondOne(g, w) else { continue }
            let (x, y) = (t % width, t / width)
            let sampled = (0 ..< samples).map { s -> [UInt32] in
                let h = ResolveRef.sampleHash(x, y, s)
                return [h & 255, h >> 8 & 255, h >> 16 & 255, h >> 24]
            }
            out.add(x: x, y: y, got: g, want: w, samples: sampled)
        }
        return out
    }
}
