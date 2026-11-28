import Foundation
import HWTestCore
import HWTestGPU
import HWTestX1
import Metal

extension X1TwodProbes {
    /// One reading of how the engine filters: an origin shift in source texels, the weights' fractional bits (0: exact)
    /// and, for sRGB, whether it filters linear light (Metal's sampler) or codes
    struct Filter: Hashable {
        let shift: Double
        let weightBits: Int
        let linearLight: Bool

        var name: String { "o\(shift == 0 ? "0" : "m")_w\(weightBits)\(linearLight ? "_lin" : "")" }

        static func all(point: Bool, srgb: Bool) -> [Filter] {
            [0.0, -0.5].flatMap { shift in
                (point ? [0] : [0, 8, 6, 4]).flatMap { bits in
                    (srgb && !point ? [false, true] : [false]).map {
                        Filter(shift: shift, weightBits: bits, linearLight: $0)
                    }
                }
            }
        }

        /// Metal's: no shift, exact weights, sRGB in linear light
        static func metal(srgb: Bool) -> Filter { Filter(shift: 0, weightBits: 0, linearLight: srgb) }
    }

    struct AxisTap {
        let i0: Int
        let w0: Double
        let i1: Int
        let w1: Double
        let two: Bool
    }

    static func axisTaps(count: Int, size: Int, du: UInt64, point: Bool, _ f: Filter) -> [AxisTap] {
        (0 ..< count).map { idx in
            let at = Double(UInt64(idx) * du + du / 2) / 4_294_967_296
            let p = at + f.shift
            if point {
                return AxisTap(i0: min(max(Int(p.rounded(.down)), 0), size - 1), w0: 1, i1: 0, w1: 0, two: false)
            }
            let c = p - 0.5
            let i = Int(c.rounded(.down))
            var w = c - Double(i)
            if f.weightBits > 0 { w = (w * Double(1 << f.weightBits)).rounded(.down) / Double(1 << f.weightBits) }
            return AxisTap(
                i0: min(max(i, 0), size - 1), w0: 1 - w, i1: min(max(i + 1, 0), size - 1), w1: w, two: true
            )
        }
    }

    static func sourceValues(_ codes: [SIMD4<UInt32>], _ format: X1Format, linearSrgb: Bool) -> [Double] {
        let channels = format.channels, chCount = channels.count
        var out = [Double](repeating: 0, count: codes.count * 4)
        out.withUnsafeMutableBufferPointer { dst in
            let p = dst.baseAddress!
            for i in 0 ..< codes.count {
                let code = codes[i], base = i * 4
                for c in 0 ..< chCount {
                    let ch = channels[c]
                    let v = ch.value(code[c])
                    p[base + c] = linearSrgb && ch == .srgb8 ? X1Channel.linear(v) : v
                }
            }
        }
        return out
    }

    /// Source texel taps and weights along one axis at 32.32 position `at` (in source texels from the corner)
    static func taps(_ at: Double, _ size: Int, point: Bool, _ f: Filter) -> [(Int, Double)] {
        let p = at + f.shift
        if point { return [(min(max(Int(p.rounded(.down)), 0), size - 1), 1)] }
        let c = p - 0.5
        let i = Int(c.rounded(.down))
        var w = c - Double(i)
        if f.weightBits > 0 { w = (w * Double(1 << f.weightBits)).rounded(.down) / Double(1 << f.weightBits) }
        return [(min(max(i, 0), size - 1), 1 - w), (min(max(i + 1, 0), size - 1), w)]
    }

    static func filtered(
        _ codes: [[UInt32]], _ width: Int, _ height: Int, _ format: X1Format, x: Double, y: Double,
        point: Bool, _ f: Filter
    ) -> [Double] {
        let tx = taps(x, width, point: point, f), ty = taps(y, height, point: point, f)
        return format.channels.enumerated().map { c, channel in
            let linear = channel == .srgb8 && f.linearLight
            var sum = 0.0
            for (j, wy) in ty {
                for (i, wx) in tx where wx * wy != 0 {
                    let v = channel.value(codes[j * width + i][c])
                    sum += wx * wy * (linear ? X1Channel.linear(v) : v)
                }
            }
            return linear ? X1Channel.encoded(sum) : sum
        }
    }

    /// Per format, the reading its bilinear cells fit best, by how many: "bgra8=o0_w8:4"
    static func weightSummary(_ weights: [String: [String: Int]]) -> String {
        X1Format.all.compactMap { f in
            guard let counts = weights[f.name] else { return nil }
            let top = counts.max { $0.value < $1.value || ($0.value == $1.value && $0.key > $1.key) }!
            return "\(f.name)=\(top.key):\(top.value)"
        }.joined(separator: ",")
    }

    /// gpu.x1.twod_scale (copy.twod, METALMAP d4/d18): scaled copies by the 2D engine, point and bilinear, corner origin
    /// at half a step (the mip chain's sampling, fx::kGm20bSampling), at 1:1, 2:1, 3:7 (up) and 3:2 (down), against
    /// the CPU's filter of the source's known codes under each reading (Filter) and rounding. A cell grades against
    /// Metal's reading and names the reading and rules that fit it with the fewest misses; the rules on the line come
    /// from each cell's best reading (`weights=` names it per format), so 3:2 and 3:7 cells are held to the engine's
    /// weights rather than exact ones.
    static let scale = X1Probe(
        id: "gpu.x1.twod_scale", name: "2D-engine scaled copies against the CPU's filter, per format, ratio and filter",
        probe: "copy.twod", risky: true, timeout: 240
    ) { gpu in
        let journal = ProbeJournal(probe: "gpu.x1.twod_scale")
        var cells: [(String, X1RuleTally)] = [], explained: [(String, X1RuleTally)] = []
        var readings: [String] = [], offPath: [String] = []
        var weights: [String: [String: Int]] = [:]
        var ops = 0
        let ratios = [("1to1", 64, 64), ("2to1", 128, 64), ("3to7", 30, 70), ("3to2", 96, 64)]
        for format in X1Format.all {
            journal.starting(format.name)
            let srgb = format.channels.contains(.srgb8)
            let channels = format.channels, chCount = channels.count
            let metalRule = format.metalRounding
            for (ratio, from, to) in ratios {
                let (src, codes) = try await sourceFlat(gpu, format, from, from)
                let srcVal = sourceValues(codes, format, linearSrgb: false)
                let srcLin = srgb ? sourceValues(codes, format, linearSrgb: true) : []
                for point in [true, false] {
                    let written = try gpu.texture(format.pixelFormat, to, to, usage: [.renderTarget, .shaderRead])
                    let du = (UInt64(from) << 32) / UInt64(to)
                    let batch = try await gpu.run(forms: "") { commands in
                        let blit = commands.makeBlitCommandEncoder()!
                        try GM20BProbe.engine(
                            commands,
                            [
                                "kind": "scale", "read": src, "written": written, "du": du, "dv": du, "sx": du / 2,
                                "sy": du / 2, "bilinear": point ? 0 : 1, "corner": 1,
                            ]
                        )
                        blit.endEncoding()
                    }
                    ops += batch.taken["ops"] ?? 0
                    let name = "\(format.name)_\(ratio)_\(point ? "point" : "linear")"
                    guard batch.path == "gm20b" else {
                        offPath.append("\(name):\(batch.path)")
                        continue
                    }
                    let bytes = try await gpu.read(written, bytesPerPixel: format.bytesPerTexel)
                    let filters = Filter.all(point: point, srgb: srgb)
                    var tallies = [X1RuleTally](repeating: X1RuleTally(), count: filters.count)
                    let metal = filters.firstIndex(of: .metal(srgb: srgb && !point)) ?? 0
                    bytes.withUnsafeBufferPointer { bp in
                        let byteBase = bp.baseAddress!
                        srcVal.withUnsafeBufferPointer { vp in
                            srcLin.withUnsafeBufferPointer { lp in
                                let valBase = vp.baseAddress!
                                let linBase = lp.baseAddress
                                tallies.withUnsafeMutableBufferPointer { tp in
                                    DispatchQueue.concurrentPerform(iterations: filters.count) { k in
                                        let f = filters[k]
                                        let taps = axisTaps(count: to, size: from, du: du, point: point, f)
                                        let useLin = f.linearLight && srgb
                                        let srcBase = useLin ? linBase! : valBase
                                        var tally = X1RuleTally()
                                        for y in 0 ..< to {
                                            let ty = taps[y]
                                            let row0 = ty.i0 * from, row1 = ty.i1 * from
                                            let tRow = y * to
                                            for x in 0 ..< to {
                                                let t = tRow + x
                                                let tx = taps[x]
                                                let got = format.codes4(byteBase, t)
                                                var exact = SIMD4<Double>.zero
                                                if !ty.two {
                                                    let o00 = (row0 + tx.i0) * 4
                                                    for c in 0 ..< chCount { exact[c] = srcBase[o00 + c] }
                                                } else {
                                                    let w00 = tx.w0 * ty.w0, w10 = tx.w1 * ty.w0
                                                    let w01 = tx.w0 * ty.w1, w11 = tx.w1 * ty.w1
                                                    let o00 = (row0 + tx.i0) * 4, o10 = (row0 + tx.i1) * 4
                                                    let o01 = (row1 + tx.i0) * 4, o11 = (row1 + tx.i1) * 4
                                                    for c in 0 ..< chCount {
                                                        var sum = 0.0
                                                        if w00 != 0 { sum += w00 * srcBase[o00 + c] }
                                                        if w10 != 0 { sum += w10 * srcBase[o10 + c] }
                                                        if w01 != 0 { sum += w01 * srcBase[o01 + c] }
                                                        if w11 != 0 { sum += w11 * srcBase[o11 + c] }
                                                        exact[c] =
                                                            useLin && channels[c] == .srgb8
                                                                ? X1Channel.encoded(sum) : sum
                                                    }
                                                }
                                                tally.add(
                                                    got, exact: exact, channels: channels, metal: metalRule, at: t
                                                )
                                            }
                                        }
                                        tp[k] = tally
                                    }
                                }
                            }
                        }
                    }
                    cells.append((name, tallies[metal]))
                    let best = tallies.indices.min { tallies[$0].fewestMisses < tallies[$1].fewestMisses } ?? metal
                    explained.append((name, tallies[best]))
                    readings.append("\(name):\(filters[best].name):\(tallies[best].text)")
                    if !point { weights[format.name, default: [:]][filters[best].name, default: 0] += 1 }
                }
            }
        }
        var finding = finding("copy.twod", cells, ops: ops, offPath: offPath, explained: explained)
        finding.metrics.set("weights", Self.weightSummary(weights))
        finding.metrics.set("best_readings", readings.joined(separator: ","))
        return finding
    }
}
