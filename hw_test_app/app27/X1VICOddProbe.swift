import Foundation
import HWTestCore
import HWTestGPU

/// DISP-35: odd-sized 4:2:0 and 4:2:2 surfaces, described to VIC one pixel wider (4:2:0: and taller) in whole chroma
/// pairs with their rectangles left odd (drivers/TegraVIC/SwitchVICOddProbe.h). '420v' and '422v' pictures copied onto
/// themselves 1:1, even-sized (the controls), then odd across, down and both, then mirrored across even and odd (_t1),
/// compared in the kernel byte for byte. <case>_last counts the wrong bytes in the last column, row or chroma pair.
enum X1VICOddProbe {
    static let magic: UInt32 = 0x564F_4444  // 'VODD'
    static let cases = 8
    static let caseWords = 12
    static let controls = 2
    static let replyBytes = 16 + 4 * caseWords * cases

    static let probe = X1Probe(
        id: "gpu.x1.vic_odd_size", name: "VIC copies odd-sized 4:2:0 and 4:2:2 in whole chroma pairs",
        probe: "DISP35", risky: true
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
            let name = "\(X1VICKernelTest.name(r[0]))_\(r[1])x\(r[2])" + (r[3] != 0 ? "_t\(r[3])" : "")
            metrics.set(name + "_status", Int(Int32(bitPattern: r[4])))
            metrics.set(name + "_last", Int(r[6]))
            metrics.set(name + "_outside", Int(r[11]))
            if r[5] > 0 { metrics.set(name + "_first", "\(r[7]):\(r[8]),\(r[9]):\(String(r[10], radix: 16))") }
            let count = Int(r[5]) + Int(r[11]) + (r[4] != 0 ? 1 : 0)
            if i >= controls { wrong[name + "_wrong"] = count } else { metrics.set(name + "_wrong", count) }
        }
        metrics.set("controls_clean", reply[2] != 0)
        let finding = ProbeFinding.counted(wrong, paths: [], metrics: metrics)
        guard reply[2] != 0, Int(reply[1]) == cases else {
            return ProbeFinding(.inconclusive, finding.metrics, note: "an even-sized copy was not exact")
        }
        return finding
    }
}
