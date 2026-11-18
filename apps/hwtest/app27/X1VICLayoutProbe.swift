import Foundation
import HWTestCore
import HWTestGPU

/// L993: kTegraVICTraits.blockLinearOut, VIC writing OutBlkKind GENERIC_16Bx2, which no caller has run
/// (drivers/TegraVIC/SwitchVICLayoutProbe.h). A BGRA card into 1, 2 and 16 GOBs a block and NV12 into NVDEC's 2, read
/// back in the kernel through the Tegra block-linear law, against pitch-linear copies of both. <case>_laws names every
/// layout the destination reads back under exactly: the case's own (bl<k> or pitch) is the claim.
enum X1VICLayoutProbe {
    static let magic: UInt32 = 0x5642_4C4B  // 'VBLK'
    static let cases = 6
    static let caseWords = 11
    static let controls = 2
    static let replyBytes = 16 + 4 * caseWords * cases

    static func laws(_ mask: UInt32) -> String {
        let names = (0..<6).filter { mask >> $0 & 1 != 0 }.map { "bl\($0)" } + (mask >> 8 & 1 != 0 ? ["pitch"] : [])
        return names.isEmpty ? "none" : names.joined(separator: "+")
    }

    static let probe = X1Probe(
        id: "gpu.x1.vic_block_linear", name: "VIC writes BGRA and NV12 block linear as the GOB law reads them",
        probe: "L993", risky: true
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
            let name = X1VICKernelTest.name(r[0]) + (r[1] == 0xff ? "_pitch" : "_bl\(r[1])")
            metrics.set(name + "_status", Int(Int32(bitPattern: r[2])))
            metrics.set(name + "_laws", laws(r[8]))
            metrics.set(name + "_pad", Int(r[9]))
            metrics.set(name + "_outside", Int(r[10]))
            if r[3] > 0 { metrics.set(name + "_first", "\(r[4]):\(r[5]),\(r[6]):\(String(r[7], radix: 16))") }
            if i >= controls { wrong[name + "_wrong"] = Int(r[3]) + Int(r[10]) + (r[2] != 0 ? 1 : 0) }
        }
        metrics.set("controls_clean", reply[2] != 0)
        let finding = ProbeFinding.counted(wrong, paths: [], metrics: metrics)
        guard reply[2] != 0, Int(reply[1]) == cases else {
            return ProbeFinding(.inconclusive, finding.metrics, note: "a pitch-linear control did not copy exactly")
        }
        return finding
    }
}
