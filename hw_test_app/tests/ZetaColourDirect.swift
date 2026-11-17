import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.zeta_colour's diagnostic arm, outside its claim and run after it (a wrong write here can kill the channel):
/// the route's draw with the colour ROP straight over the zeta's own pages (switchRawProbeZetaDirect:), the tiled cache
/// off for the draw, the configuration Gallium nvc0 draws into a ZF32_X24S8 zeta in. Boot 21 drew over those pages with
/// the tiled cache on and nothing it wrote reached memory. direct_<format>_* at 0: the tiled cache lost those writes;
/// off: they are lost untiled too, which leaves the zeta kind or the integer format (nvc0 draws R32G32_FLOAT where this
/// draws RG32Uint). Each format's resolve and turn cells at every size: direct_<format>_depth, _stencil and _tested count
/// texels off the pattern, direct_rop_draws the draws, direct_errors the parts that threw, and the test's direct_reopens
/// and direct_gpu_after the channels it cost and whether the GPU answers after it.
struct ZetaColourDirect {
    var counts: [String: ZetaColourCells.Count] = [:]
    var draws = 0, errors = 0
    var first: String?

    static func run(_ gpu: GPU, _ cells: ZetaColourCells) async -> ZetaColourDirect {
        var arm = ZetaColourDirect()
        let start = RawFacade.counters(of: gpu.device)
        var n: UInt32 = 0
        for format in ZetaColourTest.formats {
            let label = ZetaColourCells.name(format)
            var count = ZetaColourCells.Count()
            for (w, h) in ZetaColourTest.sizes {
                n += 1
                let seed = 70 + n
                do {
                    count.add(try await cells.resolve(format, w, h, seed: seed))
                    if let t = try await cells.turn(format, w, h, seed: seed + 100) { count.add(t) }
                } catch {
                    arm.errors += 1
                    if arm.first == nil { arm.first = "\(label)_\(w)x\(h): \(error)" }
                }
            }
            if arm.first == nil, let miss = count.first { arm.first = miss }
            arm.counts[label] = count
        }
        let end = RawFacade.counters(of: gpu.device)
        arm.draws = (end["zeta_colour_draws"] ?? 0) - (start["zeta_colour_draws"] ?? 0)
        return arm
    }

    func report(into metrics: inout Metrics) {
        for (label, count) in counts.sorted(by: { $0.key < $1.key }) {
            if label == "d32s8" { metrics.set("direct_d32s8_depth", count.depth) }
            metrics.set("direct_\(label)_stencil", count.stencil)
            metrics.set("direct_\(label)_tested", count.tested)
        }
        metrics.set("direct_rop_draws", draws)
        metrics.set("direct_errors", errors)
    }
}
