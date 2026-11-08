import Foundation

/// Where and how a read-back picture differs from its reference, small enough for one log line: the count, the worst
/// error, the first wrong pixel with both values, the box around every wrong pixel, an 8x8 map of where they are and
/// how many whole columns or rows went wrong (streaks). A NaN in the reference means "any value".
public struct RasterDiff: Equatable, Sendable {
    public let width: Int
    public let height: Int
    public let wrong: Int
    public let maxError: Float
    public let first: [Int]
    public let firstGot: String
    public let firstWant: String
    public let box: [Int]
    public let map: String
    public let streakColumns: Int
    public let streakRows: Int
    public let mask: [Bool]

    public var matches: Bool { wrong == 0 }

    public static func compare(_ got: Raster, _ want: Raster, tolerance: Float) -> RasterDiff {
        precondition(
            got.width == want.width && got.height == want.height && got.channels == want.channels,
            "comparing \(got.width)x\(got.height)x\(got.channels) with \(want.width)x\(want.height)x\(want.channels)")
        let (w, h) = (got.width, got.height)
        var mask = [Bool](repeating: false, count: w * h)
        var columns = [Int](repeating: 0, count: w)
        var rows = [Int](repeating: 0, count: h)
        var cells = [Bool](repeating: false, count: 64)
        var (wrong, maxError) = (0, Float(0))
        var first: [Int] = []
        var box = [Int.max, Int.max, -1, -1]
        for y in 0..<h {
            for x in 0..<w {
                var bad = false
                for c in 0..<got.channels {
                    let (g, e) = (got[x, y, c], want[x, y, c])
                    if e.isNaN { continue }
                    let error = g.isNaN ? Float.infinity : abs(g - e)
                    maxError = max(maxError, error)
                    if error > tolerance { bad = true }
                }
                guard bad else { continue }
                wrong += 1
                mask[y * w + x] = true
                columns[x] += 1
                rows[y] += 1
                cells[(y * 8 / h) * 8 + x * 8 / w] = true
                if first.isEmpty { first = [x, y] }
                box = [min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)]
            }
        }
        let map = stride(from: 0, to: 64, by: 4).map { at in
            String(cells[at..<(at + 4)].reduce(0) { $0 << 1 | ($1 ? 1 : 0) }, radix: 16)
        }.joined()
        return RasterDiff(
            width: w, height: h, wrong: wrong, maxError: maxError, first: first,
            firstGot: first.isEmpty ? "" : got.shown(got.pixel(first[0], first[1])),
            firstWant: first.isEmpty ? "" : want.shown(want.pixel(first[0], first[1])),
            box: wrong == 0 ? [] : box, map: map,
            streakColumns: columns.filter { $0 >= max(4, h / 2) }.count,
            streakRows: rows.filter { $0 >= max(4, w / 2) }.count, mask: mask)
    }

    /// One phrase for the log's why=: "37_wrong_of_4096_at_3,5_got_..._want_..._box_..._map_..."
    public var summary: String {
        guard wrong > 0 else { return "exact" }
        var parts = [
            "\(wrong) wrong of \(width * height)", "at \(first[0]),\(first[1])", "got \(firstGot)",
            "want \(firstWant)", "box \(box.map(String.init).joined(separator: ","))", "map \(map)",
        ]
        if streakColumns > 0 { parts.append("\(streakColumns) streak columns") }
        if streakRows > 0 { parts.append("\(streakRows) streak rows") }
        return parts.joined(separator: " ")
    }

    public func metrics(prefix: String = "") -> Metrics {
        var metrics = Metrics()
        metrics.set(prefix + "wrong", wrong)
        metrics.set(prefix + "maxd", Double(maxError), digits: 3)
        guard wrong > 0 else { return metrics }
        metrics.set(prefix + "at", first.map(String.init).joined(separator: ","))
        metrics.set(prefix + "got", firstGot)
        metrics.set(prefix + "want", firstWant)
        metrics.set(prefix + "box", box.map(String.init).joined(separator: ","))
        metrics.set(prefix + "map", map)
        metrics.set(prefix + "streaks", streakColumns + streakRows)
        return metrics
    }
}
