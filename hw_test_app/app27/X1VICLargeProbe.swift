import Foundation
import HWTestCore
import HWTestGPU

/// L1002 and L992: one VIC job scales and turns a 4K picture in NVDEC's layout onto the 720x1280 panel, into '420v' and
/// BGRA, and reads 4096-wide pictures block linear and pitch (drivers/TegraVIC/SwitchVICLargeProbe.h). Pictures are one
/// colour a 32x32 block; <case>_checked output pixels sit wholly inside one, <case>_wrong of them are off it,
/// <case>_unwritten still hold the poison. A 64x64 copy is the control.
enum X1VICLargeProbe {
    static let magic: UInt32 = 0x5642_494B  // 'VBIK'
    static let cases = 5
    static let caseWords = 12
    static let replyBytes = 16 + 4 * caseWords * cases
    static let names = ["copy", "4k_turn_nv12", "4k_turn_bgra", "4096_block", "4096_pitch"]

    static let probe = X1Probe(
        id: "gpu.x1.vic_4k", name: "VIC 4K scaled and turned in one job, 4096-wide sources",
        probe: "L1002/L992", risky: true, timeout: 180
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
            let count = Int(r[5]) + Int(r[6]) + Int(r[7]) + (r[3] != 0 ? 1 : 0)
            metrics.set(name + "_status", Int(Int32(bitPattern: r[3])))
            metrics.set(name + "_checked", Int(r[4]))
            metrics.set(name + "_unwritten", Int(r[6]))
            metrics.set(name + "_outside", Int(r[7]))
            if r[2] == 0x4247_5241 { metrics.set(name + "_max", Int(r[8])) }
            if r[5] > 0 { metrics.set(name + "_first", "\(r[9]),\(r[10]):\(String(r[11], radix: 16))") }
            if i > 0 { wrong[name + "_wrong"] = count } else { metrics.set(name + "_wrong", count) }
        }
        metrics.set("controls_clean", reply[2] != 0)
        let finding = ProbeFinding.counted(wrong, paths: [], metrics: metrics)
        guard reply[2] != 0, Int(reply[1]) == cases else {
            return ProbeFinding(.inconclusive, finding.metrics, note: "the 64x64 copy was not exact")
        }
        return finding
    }
}
