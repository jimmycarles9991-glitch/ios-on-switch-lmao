import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit

/// L959: kTegraVICTraits.multiSlot, one VIC job composing several slots bottom first
/// (drivers/TegraVIC/SwitchVICComposeProbe.h), the display's overlays as one plane when DC A's memory admission refuses
/// them. A 64x32 BGRA composite of small cards and a flat NV12 picture: one slot (tegra_vic_build's copy is the
/// control), disjoint slots, overlapping opaque slots both ways, premultiplied and straight per-pixel alpha, half global
/// alpha, a 2:1 scaled slot, 16-GOB block-linear output, overlapping opaque slots with the blend words all zero
/// (zero_blend: what single-slot jobs run with) and the premultiplied case with alpha fill COMPOSITED. <case>_models is the readings every pixel met (bit 0 as
/// programmed, 1 the top slot replacing, 2 the top slot missing, 3 read premultiplied, 4 read straight, 5 nothing drawn):
/// a wrong case says which reading the engine follows.
enum X1VICComposeProbe {
    static let magic: UInt32 = 0x5643_4D50  // 'VCMP'
    static let cases = 12
    static let caseWords = 10
    static let replyBytes = 16 + 4 * caseWords * cases
    static let names = [
        "control", "one", "disjoint", "over", "under", "premultiplied", "straight", "global", "scaled", "block",
        "zero_blend", "composited_fill",
    ]

    /// display.planes_composite: the display's composite route, its selector calls through this app's X1VICKernelTest.
    static let display = PlanesCompositeTest(kernelTest: { X1VICKernelTest.call($0, replyBytes: $1) })

    static let probe = X1Probe(
        id: "gpu.x1.vic_compose", name: "VIC slots composed in one job: order, alpha, scale, block-linear output",
        probe: "L959", risky: true
    ) { _ in
        var metrics = Metrics()
        let reply: [UInt32]
        switch X1VICKernelTest.call(magic, replyBytes: replyBytes) {
        case .failure(let failure): return ProbeFinding(.inconclusive, metrics, note: failure.description)
        case .success(let words): reply = words
        }
        var wrong: [String: Int] = [:]
        for i in 0..<min(Int(reply[1]), cases) {
            let r = Array(reply[(4 + caseWords * i)..<(4 + caseWords * (i + 1))])
            let name = names[i]
            let count = Int(r[3]) + Int(r[9]) + (r[1] != 0 ? 1 : 0)
            metrics.set(name + "_status", Int(Int32(bitPattern: r[1])))
            metrics.set(name + "_models", String(r[2], radix: 16))
            metrics.set(name + "_max", Int(r[4]))
            metrics.set(name + "_outside", Int(r[9]))
            if r[3] > 0 {
                metrics.set(name + "_first", "\(r[5]),\(r[6]):\(String(r[7], radix: 16))/\(String(r[8], radix: 16))")
            }
            if i > 0 { wrong[name + "_wrong"] = count } else { metrics.set(name + "_wrong", count) }
        }
        metrics.set("controls_clean", reply[2] != 0)
        let finding = ProbeFinding.counted(wrong, paths: [], metrics: metrics)
        guard reply[2] != 0, Int(reply[1]) == cases else {
            return ProbeFinding(.inconclusive, finding.metrics, note: "the single-slot copy was not exact")
        }
        return finding
    }
}
