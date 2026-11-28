import Darwin
import Foundation
import HWTestCore
import SwitchCPUClock

/// What a scene turn cost this app, to tell its own UI work from what the system gave it: the main thread's CPU over
/// the turn, the task's page faults and page-ins (TASK_EVENTS_INFO: a page-in waited for storage, as the shared cache's
/// pages do when cold), and the A57 cluster's clock before, during and after (one clock for all four cores, so a short
/// add chain on any thread reads it; cpu.clock's method).
struct TurnCost {
    struct Sample {
        let mainNs: UInt64
        let faults: Int32
        let pageins: Int32
    }

    /// About 5 ms at 1785 MHz: short enough to leave the turn its cores.
    static let clockBlocks: UInt64 = 9000

    let mainMs: Double
    let faults: Int
    let pageins: Int
    let megahertz: [Double]

    static func sample(main port: thread_act_t) -> Sample {
        var info = task_events_info()
        var count = mach_msg_type_number_t(MemoryLayout<task_events_info>.size / MemoryLayout<natural_t>.size)
        let status = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_EVENTS_INFO), $0, &count)
            }
        }
        let ok = status == KERN_SUCCESS
        return Sample(
            mainNs: ThreadTimes.nanoseconds(of: port) ?? 0, faults: ok ? info.faults : 0, pageins: ok ? info.pageins : 0
        )
    }

    static func megahertz() -> Double {
        let start = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)
        _ = switch_cpu_add_chain(clockBlocks)
        let nanoseconds = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - start
        return nanoseconds > 0 ? Double(clockBlocks) * Double(SWITCH_CPU_CLOCK_BLOCK) / Double(nanoseconds) * 1000 : 0
    }

    /// `body`, with the cluster clock read off the main thread before it, 200 ms into it and after it.
    static func around<R>(main port: thread_act_t, _ body: () async -> R) async -> (R, TurnCost) {
        let clocks = Clocks()
        let queue = DispatchQueue.global(qos: .userInitiated)
        queue.async { clocks.add(megahertz()) }
        queue.asyncAfter(deadline: .now() + 0.2) { clocks.add(megahertz()) }
        let before = sample(main: port)
        let result = await body()
        let after = sample(main: port)
        let last = await withCheckedContinuation { continuation in
            queue.async { continuation.resume(returning: megahertz()) }
        }
        clocks.add(last)
        let cost = TurnCost(
            mainMs: Double(after.mainNs - min(before.mainNs, after.mainNs)) / 1e6,
            faults: Int(after.faults) - Int(before.faults), pageins: Int(after.pageins) - Int(before.pageins),
            megahertz: clocks.all
        )
        return (result, cost)
    }

    var metric: String {
        String(format: "%.0f/%d/%d/", mainMs, faults, pageins) + megahertz.map { String(format: "%.0f", $0) }
            .joined(separator: ",")
    }

    final class Clocks: @unchecked Sendable {
        private let lock = NSLock()
        private var values: [Double] = []

        func add(_ value: Double) {
            lock.lock()
            values.append(value)
            lock.unlock()
        }

        var all: [Double] {
            lock.lock()
            defer { lock.unlock() }
            return values
        }
    }
}
