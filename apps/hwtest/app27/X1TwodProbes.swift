import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The 2D engine (FERMI_TWOD_A) per (format, operation), held to CPU rules: each cell's texels against the exact
/// value the operation means, rounded by Metal's rule and by every other rule (X1RuleTally). A cell is exact where it
/// keeps Metal's rule, near where it is within one code, and its rules name the rounding the engine followed, so the
/// planner can select a cell exactly or within a stated margin. Each probe runs its 2D work through the facade's probe
/// channel (raw_probe_ops.h) at an open blit encoder.
enum X1TwodProbes {
    /// claim_verdict: PASS when every cell keeps Metal's rule, FAIL when a cell is beyond one code or fits no rule,
    /// INCONCLUSIVE between; margin_verdict: PASS when every cell is within one code by a named rule. Cells whose
    /// command buffer left the raw path are listed apart (off_path_cells=) and judge nothing. `explained`, where given,
    /// holds each cell under the reading of the unit's arithmetic that fits it best (its weights, its order of rounding):
    /// the rules come from those.
    static func finding(
        _ rule: String, _ cells: [(String, X1RuleTally)], ops: Int, offPath: [String],
        explained: [(String, X1RuleTally)]? = nil
    ) -> ProbeFinding {
        var metrics = Metrics()
        metrics.set("rule", rule)
        metrics.set("ops", ops)
        let exact = cells.filter { $0.1.offMetal == 0 }.count
        let ruled = explained ?? cells
        let unexplained = ruled.filter { $0.1.rules.isEmpty }.map(\.0)
        let wrong = cells.filter { $0.1.beyondOne > 0 }.map(\.0)
        metrics.set("exact_cells", exact)
        metrics.set("near_cells", cells.count - exact - wrong.count)
        metrics.set("wrong_cells", wrong.count)
        metrics.set("margin_verdict", wrong.isEmpty && unexplained.isEmpty && !cells.isEmpty ? "PASS" : "FAIL")
        metrics.set("unexplained", unexplained.isEmpty ? "-" : unexplained.joined(separator: ","))
        if let first = cells.first(where: { $0.1.offMetal > 0 }) { metrics.set("first", "\(first.0)@\(first.1.first)") }
        metrics.set("rules", ruleSummary(ruled))
        metrics.set("cells", cells.map { "\($0.0):\($0.1.text)" }.joined(separator: ","))
        metrics.set("off_path_cells", offPath.isEmpty ? "-" : offPath.joined(separator: ","))
        if cells.isEmpty { return ProbeFinding(.inconclusive, metrics, note: "no cell ran on the raw path") }
        if ops == 0 { return ProbeFinding(.inconclusive, metrics, note: "the 2D work never reached a blit encoder") }
        let verdict: ClaimVerdict =
            exact == cells.count ? .pass : !wrong.isEmpty || !unexplained.isEmpty ? .fail : .inconclusive
        return ProbeFinding(verdict, metrics, note: verdict == .inconclusive ? "within one code: see rules" : "")
    }

    /// Per format, the rules every one of its cells fits: "bgra8=ties_down,rgba16f=down+ties_even"
    static func ruleSummary(_ cells: [(String, X1RuleTally)]) -> String {
        var byFormat: [(String, Set<X1Rounding>)] = []
        for (name, tally) in cells {
            let format = String(name.split(separator: "_").first ?? "")
            let rules = Set(tally.rules)
            if let i = byFormat.firstIndex(where: { $0.0 == format }) {
                byFormat[i].1.formIntersection(rules)
            } else {
                byFormat.append((format, rules))
            }
        }
        return byFormat.map { f in
            let r = X1Rounding.allCases.filter { f.1.contains($0) }.map(\.rawValue)
            return "\(f.0)=\(r.isEmpty ? "none" : r.joined(separator: "+"))"
        }.joined(separator: ",")
    }

    /// A code the channel holds, from a hash: unorm and sRGB any code, floats in [0, 1)
    static func code(_ channel: X1Channel, _ hash: UInt32) -> UInt32 {
        switch channel {
        case .unorm(let bits): return hash & ((1 << UInt32(bits)) - 1)
        case .srgb8: return hash & 255
        case .float(let m): return hash % (15 << UInt32(m))
        case .float32: return Float(Double(hash & 0xffff) / 65535).bitPattern
        }
    }

    /// A w x h source of `format` from per-texel hashed codes; its codes, RGBA order, texel by texel
    static func source(_ gpu: GPU, _ format: X1Format, _ w: Int, _ h: Int) async throws -> (MTLTexture, [[UInt32]]) {
        let t = try gpu.texture(format.pixelFormat, w, h, usage: [.renderTarget, .shaderRead])
        var codes: [[UInt32]] = []
        var bytes: [UInt8] = []
        for i in 0..<w * h {
            let texel = format.channels.enumerated().map { c, channel in
                code(channel, probeHash(UInt32(i % w), UInt32(i / w), UInt32(7 + c)))
            }
            codes.append(texel)
            bytes += format.bytes(texel)
        }
        try await gpu.upload(bytes, to: t, bytesPerRow: w * format.bytesPerTexel)
        return (t, codes)
    }
}

extension X1Format {
    /// Metal's rounding for the format's stores: nearest for unorm and sRGB codes, ties to even for floats
    var metalRounding: X1Rounding {
        channels.contains {
            if case .float = $0 { return true }; return $0 == .float32
        } ? .tiesEven : .nearest
    }
}
