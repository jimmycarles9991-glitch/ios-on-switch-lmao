import Foundation
import HWTestCore

/// One serial hierarchy driven the way locationd's silos drive their cohort queue: waiters (here plain threads,
/// workqueue threads in system.dispatch_sync_workloops) each dispatch_sync onto a serial queue whose target is one
/// shared serial queue, while blocks async'd onto the shared queue keep a workqueue thread draining it and handing it
/// to the waiters. The shared queue targets a root queue, so it is its own workloop and a contended sync waits in
/// kevent_id (NOTE_WL_SYNC_WAIT) until the drainer hands it over.
final class SyncHierarchy: @unchecked Sendable {
    let shared: DispatchQueue
    let silos: [DispatchQueue]
    /// Held for the whole of every block: a try that fails is a second block running inside the hierarchy at once.
    private let inside = NSLock()
    private let lock = NSLock()
    private var overlaps = 0
    private var syncs = 0
    private var asyncs = 0

    init(silos count: Int) {
        let shared = DispatchQueue(label: "com.brunostjohn.switch.hwtest.cohort", qos: .userInitiated)
        self.shared = shared
        silos = (0..<count).map { DispatchQueue(label: "com.brunostjohn.switch.hwtest.silo\($0)", target: shared) }
    }

    func block(sync: Bool, spinMicroseconds: Double = 0) {
        let alone = inside.try()
        if spinMicroseconds > 0 {
            let end = Date(timeIntervalSinceNow: spinMicroseconds / 1_000_000)
            while Date() < end {}
        }
        if alone { inside.unlock() }
        lock.lock()
        if !alone { overlaps += 1 }
        if sync { syncs += 1 } else { asyncs += 1 }
        lock.unlock()
    }

    var totals: (syncs: Int, asyncs: Int, overlaps: Int) {
        lock.lock()
        defer { lock.unlock() }
        return (syncs, asyncs, overlaps)
    }
}

/// locationd has crashed on iOS 27 at startup since 11-08 (boots 15 to 18): its cohort queue's drainer reaches a
/// dispatch_sync waiter's context after the waiter's thread has left it, so it reads a dead or reused stack (a voucher
/// that is a constant CFString, a next pointer in an unmapped stack, a drain lock owner that names no thread). Each
/// generation's threads exit before the next start, so a context left linked in the queue lies in a stack that is gone.
struct DispatchSyncTest: HWTest {
    let id = "system.dispatch_sync"
    let name = "dispatch_sync waiters on one serial hierarchy from threads that come and go"
    let subsystem = Subsystem.system
    var timeout: TimeInterval { 150 }

    static let (generations, threads, iterations, asyncEvery) = (4, 8, 1_500, 8)

    func run(_ context: TestContext) async throws -> Outcome {
        let hierarchy = SyncHierarchy(silos: Self.threads)
        let start = Date()
        var finished = 0
        while finished < Self.generations, Self.generation(hierarchy) {
            finished += 1
        }
        if finished == Self.generations { hierarchy.shared.sync {} }
        let seconds = Date().timeIntervalSince(start)
        let (syncs, asyncs, overlaps) = hierarchy.totals
        let wantSyncs = Self.generations * Self.threads * Self.iterations
        let wantAsyncs = Self.generations * Self.threads * (Self.iterations / Self.asyncEvery)

        var metrics = Metrics()
        metrics.set("generations", Self.generations)
        metrics.set("threads", Self.threads)
        metrics.set("syncs", syncs)
        metrics.set("asyncs", asyncs)
        metrics.set("overlaps", overlaps)
        metrics.set("finished", finished)
        metrics.set("us_per_sync", seconds * 1_000_000 / Double(max(syncs, 1)), digits: 1)
        return .judged(
            [
                ("every generation's threads finished, each within 60 s", finished == Self.generations),
                ("every sync block ran exactly once (\(wantSyncs))", syncs == wantSyncs),
                ("every async block ran exactly once (\(wantAsyncs))", asyncs == wantAsyncs),
                ("no two blocks of the serial hierarchy ran at once (overlaps 0)", overlaps == 0),
            ], metrics)
    }

    /// One generation: `threads` new threads, each syncing onto its own silo; whether they all finished.
    static func generation(_ hierarchy: SyncHierarchy) -> Bool {
        let group = DispatchGroup()
        for index in 0..<threads {
            group.enter()
            let thread = Thread {
                let silo = hierarchy.silos[index]
                for step in 1...iterations {
                    silo.sync { hierarchy.block(sync: true) }
                    if step % asyncEvery == 0 {
                        hierarchy.shared.async { hierarchy.block(sync: false, spinMicroseconds: 20) }
                    }
                }
                group.leave()
            }
            thread.qualityOfService = .userInitiated
            thread.start()
        }
        return group.wait(timeout: .now() + 60) == .success
    }
}
