import Foundation
import HWTestCore
import HWTestGPU

/// L1000: kTegraVICTraits.packedYcc, VIC reading and writing '2vuy' and 'yuvs'
/// (drivers/TegraVIC/SwitchVICPackedProbe.h). Each format in against NV12 out, NV12 in against it out, and to itself,
/// with the row read as one element a pixel (_e2, TegraVICFormats.h's reading) and as one a pixel pair (_e4), then to
/// BGRA under the pixel reading, compared in the kernel. <case>_roles is what each byte of a pixel pair carried (nibble
/// p: 0 even luma, 1 odd luma, 2 Cb, 3 Cr; packed to packed, the source byte); '2vuy' should read 1302 and 'yuvs' 3120.
/// reading= names the row reading that held.
enum X1VICPackedProbe {
    static let magic: UInt32 = 0x5650_4B44  // 'VPKD'
    static let cases = 15
    static let caseWords = 12
    static let replyBytes = 16 + 4 * caseWords * cases

    static let probe = X1Probe(
        id: "gpu.x1.vic_packed422", name: "VIC packed 4:2:2 in and out against NV12, BGRA and itself", probe: "L1000",
        risky: true
    ) { _ in
        var metrics = Metrics()
        let reply: [UInt32]
        switch X1VICKernelTest.call(magic, replyBytes: replyBytes) {
        case .failure(let failure): return ProbeFinding(.inconclusive, metrics, note: failure.description)
        case .success(let words): reply = words
        }
        var wrong: [String: Int] = [:]
        var held: [UInt32: Bool] = [2: true, 4: true]
        for i in 1..<min(Int(reply[1]), cases) {
            let r = Array(reply[(4 + caseWords * i)..<(4 + caseWords * (i + 1))])
            let name = "\(X1VICKernelTest.name(r[0]))_\(X1VICKernelTest.name(r[1]))_e\(r[2])"
            let count = Int(r[4]) + Int(r[11]) + (r[3] != 0 ? 1 : 0)
            metrics.set(name + "_status", Int(Int32(bitPattern: r[3])))
            metrics.set(name + "_roles", String(r[6], radix: 16))
            metrics.set(name + "_outside", Int(r[11]))
            if r[1] == 0x4247_5241 { metrics.set(name + "_max", Int(r[5])) }
            if r[4] > 0 { metrics.set(name + "_first", "\(r[7]):\(r[8]),\(r[9]):\(String(r[10], radix: 16))") }
            if r[2] == 2 { wrong[name + "_wrong"] = count } else { metrics.set(name + "_wrong", count) }
            if count != 0 && r[1] != 0x4247_5241 { held[r[2]] = false }
        }
        let reading = held[2]! ? (held[4]! ? "both" : "pixel") : (held[4]! ? "pair" : "neither")
        metrics.set("reading", reading)
        metrics.set("controls_clean", reply[2] != 0)
        let finding = ProbeFinding.counted(wrong, paths: [], metrics: metrics)
        guard reply[2] != 0, Int(reply[1]) == cases else {
            return ProbeFinding(.inconclusive, finding.metrics, note: "the NV12 copy was not exact")
        }
        return finding
    }
}
