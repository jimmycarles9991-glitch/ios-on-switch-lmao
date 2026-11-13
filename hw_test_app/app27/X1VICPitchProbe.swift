import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit

/// LIMITS V9: VIC source rows on multiples of 256 bytes (kTegraVICTraits.sourcePitchAlign), from a deleted boot
/// self-test where 144- and 192-byte rows lost half the copy. Every public path bounces other source pitches through a
/// GPU copy first, so the probe is TegraVIC's DEVELOPMENT kernel-test selector
/// (drivers/TegraVIC/SwitchVICPitchProbe.h): nine 1:1 BGRA copies with no bounce at 320, 576 and 2880 bytes a row
/// against 256-multiple controls, each compared in the kernel.
enum X1VICPitchProbe {
    static let magic: UInt32 = 0x5650_4954  // 'VPIT'
    static let replyBytes = 340
    static let cases = 9
    static let firstControl = 6

    static let probe = X1Probe(
        id: "gpu.x1.vic_pitch", name: "VIC rows of 320, 576 and 2880 bytes copied with no bounce", probe: "V9",
        risky: true
    ) { _ in
        var metrics = Metrics()
        let reply: [UInt32]
        switch X1VICKernelTest.call(magic, replyBytes: replyBytes) {
        case .failure(let failure): return ProbeFinding(.inconclusive, metrics, note: failure.description)
        case .success(let words): reply = words
        }
        var wrong: [String: Int] = [:]
        for i in 0..<cases {
            let r = Array(reply[(4 + 9 * i)..<(4 + 9 * (i + 1))])
            let name = "p\(r[0])_\(r[1])"
            metrics.set(name + "_status", Int(Int32(bitPattern: r[3])))
            metrics.set(name + "_outside", Int(r[8]))
            if r[4] > 0 { metrics.set(name + "_first", "\(r[5]),\(r[6]):\(String(r[7], radix: 16))") }
            if i < firstControl { wrong[name + "_wrong"] = Int(r[4]) + Int(r[8]) + (r[3] != 0 ? 1 : 0) }
        }
        metrics.set("controls_clean", reply[2] != 0)
        let finding = ProbeFinding.counted(wrong, paths: [], metrics: metrics)
        guard reply[2] != 0 else {
            return ProbeFinding(.inconclusive, finding.metrics, note: "a 256-multiple control did not copy exactly")
        }
        return finding
    }
}
