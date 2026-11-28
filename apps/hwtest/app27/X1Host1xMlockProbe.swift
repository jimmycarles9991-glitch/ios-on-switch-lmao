import Foundation
import HWTestCore
import HWTestGPU

/// Host1x item 5 (drivers/TegraHost1x/SwitchHost1xMlockProbe.h): whether ACQUIRE_MLOCK (opcode 14, which TRM 14.7 does
/// not list) holds a second host1x channel on this unit, before VIC gets a second channel that relies on it.
/// control_side_by_side: without the lock the two channels ran side by side. held_off: the second channel waited while
/// the first held MLOCK 10, watched for held_us. after_release: it ran once the lock was let go. settled: no stream
/// was left for a teardown.
enum X1Host1xMlockProbe {
    static let magic: UInt32 = 0x564D_4C4B // 'VMLK'
    static let replyBytes = 32

    static let probe = X1Probe(
        id: "gpu.x1.host1x_mlock", name: "Host1x module lock 10 holds a second channel until it is let go", probe: "A3"
    ) { _ in
        var metrics = Metrics()
        let reply: [UInt32]
        switch X1VICKernelTest.call(magic, replyBytes: replyBytes) {
        case .failure(let failure): return ProbeFinding(.inconclusive, metrics, note: failure.description)
        case .success(let words): reply = words
        }
        let status = Int(Int32(bitPattern: reply[1]))
        metrics.set("status", status)
        metrics.set("control_side_by_side", reply[2] != 0)
        metrics.set("held_taken", reply[3] != 0)
        metrics.set("held_off", reply[4] != 0)
        metrics.set("after_release", reply[5] != 0)
        metrics.set("held_us", Int(reply[6]))
        metrics.set("settled", reply[7] != 0)
        guard status == 0 else {
            let why = status == -16 ? "another probe holds the probes' syncpoints" : "host1x refused it"
            return ProbeFinding(.inconclusive, metrics, note: "the probe did not run: \(why) (\(status))")
        }
        guard reply[2] != 0 else {
            return ProbeFinding(.inconclusive, metrics, note: "the control did not run two channels side by side")
        }
        let holds = reply[3] != 0 && reply[4] != 0 && reply[5] != 0 && reply[7] != 0
        return ProbeFinding(holds ? .pass : .fail, metrics)
    }
}
