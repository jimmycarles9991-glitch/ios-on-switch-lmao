import Foundation
import HWTestCore
import SwitchSyncHandoff

/// libdispatch's sync hand-offs in the shapes iOS 27's daemons crash in (dispatch/switch_sync_handoff.h). A
/// dispatch_sync waiter that leaves its wait while its context is still linked into the queue reads here as a block
/// that finds another thread inside its queue or workloop (that thread then parks there), a queue whose counter is off
/// by the blocks it lost or ran twice, an item whose voucher is not its producer's, a fault in libdispatch caught and
/// parked, or a wedge the probe reports from its own deadline with the threads inside and waiting per queue. Run on
/// iOS 17 too: untranslated arm64 libdispatch on the same A57 and our arm64 kernel code, so 17 passing and 27 failing
/// clears the CPU's exclusives, both failing points at them or at kernel code both trees share.
struct DispatchSyncHandoffTest: HWTest {
    let id = "dispatch.sync_handoff"
    let name = "dispatch_sync hand-offs across workloops, retargets, barriers and vouchers"
    let subsystem = Subsystem.system
    var timeout: TimeInterval { 120 }

    static let (threads, syncs, producers, items, rounds, deadlineMs) = (16, 20_000, 4, 5_000, 16, 60_000)

    func run(_ context: TestContext) async throws -> Outcome {
        var config = switch_sync_config(
            sync_threads: UInt32(Self.threads), syncs_per_thread: UInt32(Self.syncs),
            producers: UInt32(Self.producers), items_per_producer: UInt32(Self.items),
            retarget_rounds: UInt32(Self.rounds), deadline_ms: UInt32(Self.deadlineMs), inject: SWITCH_SYNC_BREAK_NONE
        )
        var r = switch_sync_result()
        guard switch_sync_handoff(&config, &r) else { throw TestFailure("the probe's threads could not be made") }
        let sent = UInt64(Self.producers * Self.items)

        var metrics = Metrics()
        metrics.set("finished", r.finished)
        metrics.set("elapsed_ms", r.elapsed_ms, digits: 1)
        metrics.set("syncs", Int(r.syncs))
        metrics.set("nested", Int(r.nested))
        metrics.set("barriers", Int(r.barriers))
        metrics.set("async_and_waits", Int(r.async_and_waits))
        metrics.set("servicer_syncs", Int(r.servicer_syncs))
        metrics.set("inactive_waiters", Int(r.inactive_waiters))
        metrics.set("retarget_rounds", Int(r.retarget_rounds))
        metrics.set("retarget_parked", Int(r.retarget_parked))
        metrics.set("retarget_crossed", Int(r.retarget_crossed))
        metrics.set("async_sent", Int(r.async_sent))
        metrics.set("async_received", Int(r.async_received))
        metrics.set("overlaps", Int(r.overlaps))
        metrics.set("counter_mismatches", Int(r.counter_mismatches))
        metrics.set("duplicates", Int(r.duplicates))
        metrics.set("out_of_order", Int(r.out_of_order))
        metrics.set("activity_mismatches", Int(r.activity_mismatches))
        metrics.set("activities", Int(r.activities))
        metrics.set("parked", Int(r.parked))
        metrics.set("faults", Int(r.faults))
        if r.faults > 0 {
            metrics.set("fault", "sig\(r.fault_signal)_pc_0x\(String(r.fault_pc, radix: 16))_tid_0x"
                + String(r.fault_tid, radix: 16))
        }
        if r.overlaps > 0 { metrics.set("first_overlap", Self.text(r.first_overlap)) }
        if !r.finished { metrics.set("wedge", Self.text(r.wedge)) }

        return .judged(
            [
                ("every thread and item finished within \(Self.deadlineMs / 1000) s, none parked", r.finished),
                ("no two threads inside one serial queue or workloop at once (overlaps 0)", r.overlaps == 0),
                ("every queue ran exactly the blocks called on it (counter mismatches 0)", r.counter_mismatches == 0),
                ("every async item ran (received = sent = \(sent))", r.async_received == sent && r.async_sent == sent),
                ("no item ran twice (duplicates 0)", r.duplicates == 0),
                ("each producer's items ran in order on each queue (out of order 0)", r.out_of_order == 0),
                ("every block ran under its caller's activity (activity mismatches 0)", r.activity_mismatches == 0),
                (
                    "every sync thread and producer had an activity, so every item carried a voucher",
                    Int(r.activities) == Self.threads + Self.producers
                ),
                ("no signal in libdispatch or the probe's threads (faults 0)", r.faults == 0),
                ("a waiter was woken from the other workloop (retarget crossed > 0)", r.retarget_crossed > 0),
            ], metrics
        )
    }

    /// A NUL-terminated char array the probe filled.
    static func text<T>(_ field: T) -> String {
        withUnsafeBytes(of: field) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
    }
}
