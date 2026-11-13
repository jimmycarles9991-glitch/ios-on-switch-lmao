import Foundation
import HWTestCore

/// system.dispatch_sync with locationd's waiters as its crash reports show them: workqueue threads draining a silo's
/// own serial queue (a workloop of its own) that dispatch_sync into another workloop's hierarchy, the cohort. Such a
/// waiter is the bound servicer of one kqworkloop while it waits in kevent_id(NOTE_WL_SYNC_WAIT) on another, a kernel
/// path system.dispatch_sync's plain threads and system.workloop_sync's probe never take.
struct DispatchSyncWorkloopTest: HWTest {
    let id = "system.dispatch_sync_workloops"
    let name = "dispatch_sync waiters that are workloop threads, into another workloop's serial hierarchy"
    let subsystem = Subsystem.system
    var timeout: TimeInterval { 150 }

    static let (homes, iterations, asyncEvery, rounds) = (8, 1_500, 8, 4)

    func run(_ context: TestContext) async throws -> Outcome {
        let hierarchy = SyncHierarchy(silos: Self.homes)
        let homes = (0..<Self.homes).map {
            DispatchQueue(label: "com.brunostjohn.switch.hwtest.home\($0)", qos: .userInitiated)
        }
        let start = Date()
        var finished = 0
        while finished < Self.rounds, Self.round(hierarchy, homes) {
            finished += 1
        }
        if finished == Self.rounds { hierarchy.shared.sync {} }
        let seconds = Date().timeIntervalSince(start)
        let (syncs, asyncs, overlaps) = hierarchy.totals
        let wantSyncs = Self.rounds * Self.homes * Self.iterations
        let wantAsyncs = Self.rounds * Self.homes * (Self.iterations / Self.asyncEvery)

        var metrics = Metrics()
        metrics.set("rounds", Self.rounds)
        metrics.set("homes", Self.homes)
        metrics.set("syncs", syncs)
        metrics.set("asyncs", asyncs)
        metrics.set("overlaps", overlaps)
        metrics.set("finished", finished)
        metrics.set("us_per_sync", seconds * 1_000_000 / Double(max(syncs, 1)), digits: 1)
        return .judged(
            [
                ("every round finished, each within 60 s", finished == Self.rounds),
                ("every sync block ran exactly once (\(wantSyncs))", syncs == wantSyncs),
                ("every async block ran exactly once (\(wantAsyncs))", asyncs == wantAsyncs),
                ("no two blocks of the serial hierarchy ran at once (overlaps 0)", overlaps == 0),
            ], metrics)
    }

    /// One round: each home queue's block syncs onto its silo `iterations` times; whether every block finished.
    static func round(_ hierarchy: SyncHierarchy, _ homes: [DispatchQueue]) -> Bool {
        let group = DispatchGroup()
        for (index, home) in homes.enumerated() {
            home.async(group: group) {
                let silo = hierarchy.silos[index]
                for step in 1...iterations {
                    silo.sync { hierarchy.block(sync: true) }
                    if step % asyncEvery == 0 {
                        hierarchy.shared.async { hierarchy.block(sync: false, spinMicroseconds: 20) }
                    }
                }
            }
        }
        return group.wait(timeout: .now() + 60) == .success
    }
}
