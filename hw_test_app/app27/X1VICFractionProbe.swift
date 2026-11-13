import Foundation
import HWTestCore
import HWTestGPU

/// L996: whether VIC honours a 16.16 source offset (IOSurfaceAcceleratorCapabilitiesFractionalXYOffsetsSupported) and
/// AlphaFillMode BACKGROUND (AlphaOverride), from drivers/TegraVIC/SwitchVICFractionProbe.h: 1:1 2-tap copies a quarter,
/// half and three quarters of a pixel across, down and both, each scored in the kernel against the interpolation, the
/// truncated and the rounded offset; then a varying-alpha copy with every alpha overridden. <case>_answer names the
/// answer the engine gave. fractional= decides the capability, alpha_override= the request flag.
enum X1VICFractionProbe {
    static let magic: UInt32 = 0x5646_5241  // 'VFRA'
    static let cases = 10
    static let caseWords = 13
    static let replyBytes = 16 + 4 * caseWords * cases

    static func caseName(_ r: [UInt32]) -> String {
        switch r[2] {
        case 1: return "alpha_source"
        case 2: return "alpha_background"
        default: return "q\(r[0] / 0x4000)_\(r[1] / 0x4000)"
        }
    }

    static func answer(_ r: [UInt32]) -> String {
        let names = r[2] == 0 ? ["lerp", "trunc", "round"] : ["claim", "source", "opaque"]
        return (0..<3).first { r[4 + $0] == 0 }.map { names[$0] } ?? "none"
    }

    static let probe = X1Probe(
        id: "gpu.x1.vic_fractional", name: "VIC 16.16 source offsets interpolate, and alpha override fills alpha",
        probe: "L996", risky: true
    ) { _ in
        var metrics = Metrics()
        let reply: [UInt32]
        switch X1VICKernelTest.call(magic, replyBytes: replyBytes) {
        case .failure(let failure): return ProbeFinding(.inconclusive, metrics, note: failure.description)
        case .success(let words): reply = words
        }
        var wrong: [String: Int] = [:]
        var fractional = true
        var alphaOverride = true
        for i in 0..<min(Int(reply[1]), cases) {
            let r = Array(reply[(4 + caseWords * i)..<(4 + caseWords * (i + 1))])
            let name = caseName(r)
            metrics.set(name + "_status", Int(Int32(bitPattern: r[3])))
            metrics.set(name + "_answer", answer(r))
            metrics.set(name + "_max", Int(r[7]))
            metrics.set(name + "_edges", Int(r[8]))
            metrics.set(name + "_outside", Int(r[12]))
            if r[4] > 0 { metrics.set(name + "_first", "\(r[9]),\(r[10]):\(String(r[11], radix: 16))") }
            let count = Int(r[4]) + Int(r[12]) + (r[3] != 0 ? 1 : 0)
            let control = (r[2] == 0 && r[0] == 0 && r[1] == 0) || r[2] == 1
            if !control { wrong[name + "_wrong"] = count }
            if r[2] == 0 && !control && count != 0 { fractional = false }
            if r[2] == 2 && count != 0 { alphaOverride = false }
        }
        metrics.set("controls_clean", reply[2] != 0)
        metrics.set("fractional", fractional ? "pass" : "fail")
        metrics.set("alpha_override", alphaOverride ? "pass" : "fail")
        let finding = ProbeFinding.counted(wrong, paths: [], metrics: metrics)
        guard reply[2] != 0, Int(reply[1]) == cases else {
            return ProbeFinding(
                .inconclusive, finding.metrics, note: "the integer-offset or source-alpha copy was wrong")
        }
        return finding
    }
}
