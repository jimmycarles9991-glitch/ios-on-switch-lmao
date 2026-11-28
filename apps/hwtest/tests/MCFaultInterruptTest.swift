import Foundation
import HWTestCore
import HWTestKit
import HWTestShared

/// The memory controller's error interrupt delivers (TegraVIC's MC fault probe, debug kernels): a VIC copy reads a
/// page VIC's directory leaves empty, the SMMU faults each read, and the interrupt's filter takes those faults as
/// expected instead of panicking (drivers/TegraSMMU/TegraMCErrors.cpp). A fault the interrupt never delivers panics
/// the kernel as a lost interrupt, so this test's failure is a FAIL line or that panic, never silence.
struct MCFaultInterruptTest: HWTest {
    let id = "system.mc_fault_interrupt"
    let name = "A deliberate SMMU fault reaches the MC error interrupt"
    let subsystem = Subsystem.system

    static let notPermitted = "e00002e2"

    func run(_ context: TestContext) async throws -> Outcome {
        let bytes: [UInt8]
        switch VICKernelTest.call(MCFaultReply.request, replyBytes: MCFaultReply.size) {
        case .failure(let failure):
            if failure.description.contains(Self.notPermitted) {
                return .skip("a release kernel has no MC fault probe")
            }
            return .fail("probe call: \(failure.description)")
        case .success(let reply): bytes = reply
        }
        guard let reply = MCFaultReply(bytes) else { return .fail("malformed reply") }
        var metrics = Metrics()
        metrics.set("status", Int(reply.status))
        metrics.set("attached", reply.attached)
        metrics.set("taken", reply.taken)
        metrics.set("iova", "0x" + String(reply.iova, radix: 16))
        metrics.set("intstatus", "0x" + String(reply.intstatus, radix: 16))
        metrics.set("err_status", "0x" + String(reply.errStatus, radix: 16))
        metrics.set("err_adr", "0x" + String(reply.address, radix: 16))
        if let failure = reply.failure { return .fail(failure, metrics) }
        return .pass(metrics)
    }
}
