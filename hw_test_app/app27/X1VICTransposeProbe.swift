import Foundation
import HWTestCore
import HWTestGPU

/// L998: a transposed '422v' output described before the transpose as N422R with its siting exchanged
/// (drivers/TegraVIC/SwitchVICTransposeProbe.h). A 16x16 '422v' turned as built, sited Center and Left and by transform
/// 4, then with N422 for N422R and the siting left unexchanged, beside the transposed '420v' and untransposed '422v'
/// copies. <case>_offset is where each output CbCr pair was sampled, in half columns past the pair's first (0, 1, 2; 15
/// mixed); <case>_expected what the case's reading predicts. reading= names the reading whose cases came out clean.
enum X1VICTransposeProbe {
    static let magic: UInt32 = 0x5654_3432  // 'VT42'
    static let cases = 8
    static let caseWords = 10
    static let replyBytes = 16 + 4 * caseWords * cases
    static let names = [
        "copy420_turned", "copy422", "shipped", "shipped_left", "siting_kept", "n422", "n422_kept",
        "shipped_turn4",
    ]
    static let shipped = [2, 3, 7]

    static let probe = X1Probe(
        id: "gpu.x1.vic_transpose422", name: "VIC transposed 4:2:2 out: N422R and its siting against N422",
        probe: "L998", risky: true
    ) { _ in
        var metrics = Metrics()
        let reply: [UInt32]
        switch X1VICKernelTest.call(magic, replyBytes: replyBytes) {
        case .failure(let failure): return ProbeFinding(.inconclusive, metrics, note: failure.description)
        case .success(let words): reply = words
        }
        var wrong: [String: Int] = [:]
        let clean = reply[3]
        for i in 0..<min(Int(reply[1]), cases) {
            let r = Array(reply[(4 + caseWords * i)..<(4 + caseWords * (i + 1))])
            let name = names[i]
            let count = Int(r[2]) + Int(r[3]) + Int(r[6]) + (r[1] != 0 ? 1 : 0)
            metrics.set(name + "_status", Int(Int32(bitPattern: r[1])))
            metrics.set(name + "_luma", Int(r[2]))
            metrics.set(name + "_offset", Int(r[4]))
            metrics.set(name + "_expected", Int(r[5]))
            if r[3] > 0 { metrics.set(name + "_first", "\(r[7]),\(r[8]):\(String(r[9], radix: 16))") }
            if shipped.contains(i) { wrong[name + "_wrong"] = count } else { metrics.set(name + "_wrong", count) }
        }
        let n422r = clean & 0x8C == 0x8C, n422 = clean & 0x60 == 0x60
        metrics.set("reading", n422r ? (n422 ? "both" : "n422r") : (n422 ? "n422" : "neither"))
        metrics.set("controls_clean", reply[2] != 0)
        let finding = ProbeFinding.counted(wrong, paths: [], metrics: metrics)
        guard reply[2] != 0, Int(reply[1]) == cases else {
            return ProbeFinding(.inconclusive, finding.metrics, note: "a control copy was not exact")
        }
        return finding
    }
}
