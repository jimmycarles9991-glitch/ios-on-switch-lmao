import Foundation
import HWTestCore
import HWTestKit
import HWTestShared

/// host1x's general interrupt delivers and a stopped channel comes back (drivers/TegraHost1x/SwitchHost1xErrorProbe.h,
/// debug kernels): spare channel 13 is fed opcode 15, the interrupt delivers its illegal opcode, and after the teardown
/// and restart every queue's cancellation uses, an increment-only stream runs on it. An illegal opcode that latched and
/// was never delivered panics the kernel, so a lost interrupt is that panic, never silence.
struct Host1xIllegalOpcodeTest: HWTest {
    let id = "system.host1x_illegal_opcode"
    let name = "An illegal opcode reaches host1x's general interrupt and its channel comes back"
    let subsystem = Subsystem.system

    static let notPermitted = "e00002e2"

    func run(_ context: TestContext) async throws -> Outcome {
        let bytes: [UInt8]
        switch VICKernelTest.call(Host1xErrorReply.request, replyBytes: Host1xErrorReply.size) {
        case .failure(let failure):
            if failure.description.contains(Self.notPermitted) {
                return .skip("a release kernel has no illegal-opcode probe")
            }
            return .fail("probe call: \(failure.description)")
        case .success(let reply): bytes = reply
        }
        guard let reply = Host1xErrorReply(bytes) else { return .fail("malformed reply") }
        var metrics = Metrics()
        metrics.set("attached", reply.attached)
        metrics.set("status", Int(reply.status))
        metrics.set("taken", reply.taken)
        metrics.set("recovered", reply.recovered)
        metrics.set("stat_before", "0x" + String(reply.statBefore, radix: 16))
        metrics.set("stat_stopped", "0x" + String(reply.statStopped, radix: 16))
        metrics.set("stat_restarted", "0x" + String(reply.statRestarted, radix: 16))
        // While stopped and masked: whether the channel latched again, and whether HINTSTATUS bit 31 shows it (O7).
        metrics.set("hintstatus_stopped", "0x" + String(reply.stopped.hintstatus, radix: 16))
        metrics.set("hintstatus_ext_stopped", "0x" + String(reply.stopped.hintstatusExt, radix: 16))
        metrics.set("hintmask_ext_stopped", "0x" + String(reply.stopped.hintmaskExt, radix: 16))
        metrics.set("intmask_after", "0x" + String(reply.after.intmask, radix: 16))
        if let failure = reply.failure { return .fail(failure, metrics) }
        return .pass(metrics)
    }
}
