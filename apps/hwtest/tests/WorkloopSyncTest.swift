import Foundation
import HWTestCore
import SwitchWorkloopProbe

/// The kernel half of dispatch_sync, without libdispatch: waiter threads park in kevent_id(NOTE_WL_SYNC_WAIT) on one
/// workloop and a waker hands it to each in turn (cpu/switch_workloop_probe.c), generations of threads that exit
/// between rounds as locationd's silos do. None of the waiting path is dyld cache code or a64x stub, so where
/// system.dispatch_sync fails and this passes, the stale waiter behind locationd's crashes comes from the translated
/// libdispatch; where this fails too, from the kernel's workloops.
struct WorkloopSyncTest: HWTest {
    let id = "system.workloop_sync"
    let name = "Workloop sync waits and wakes from threads that come and go, without libdispatch"
    let subsystem = Subsystem.system
    var timeout: TimeInterval { 120 }

    static let (generations, waiters, rounds) = (4, 8, 200)

    func run(_ context: TestContext) async throws -> Outcome {
        var result = switch_workloop_result()
        let ran = switch_workloop_probe(
            UInt32(Self.generations), UInt32(Self.waiters), UInt32(Self.rounds), false, &result)
        let handoffs = Self.generations * Self.waiters * Self.rounds

        var metrics = Metrics()
        metrics.set("handoffs", Int(result.handoffs))
        metrics.set("early", Int(result.early))
        metrics.set("parked", Int(result.parked))
        metrics.set("interrupted", Int(result.interrupted))
        metrics.set("errors", Int(result.errors))
        metrics.set("first_error", Int(result.first_error))
        metrics.set("stuck", Int(result.stuck))
        return .judged(
            [
                ("every generation's threads started", ran),
                ("every handoff made (\(handoffs))", Int(result.handoffs) == handoffs),
                ("no waiter left its wait before its handoff (early 0)", result.early == 0),
                ("no kevent_id error (errors 0)", result.errors == 0),
                ("the waker never waited 10 s (stuck 0)", result.stuck == 0),
                ("most waits parked in the kernel", Int(result.parked) * 2 > handoffs),
            ], metrics)
    }
}
