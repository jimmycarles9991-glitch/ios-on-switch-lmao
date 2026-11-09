import Darwin
import Foundation
import HWTestCore
import SwitchSchedProbe

/// Threads for cpu.sched (cpu/switch_sched_probe.c): each runs its C loop on a thread of its own at a given class.
enum SchedProbe {
    static let ticksPerMicrosecond: Double = {
        var base = mach_timebase_info_data_t()
        mach_timebase_info(&base)
        return Double(base.denom) / Double(base.numer) * 1000
    }()

    static func ticks(microseconds: Double) -> UInt64 { UInt64(microseconds * ticksPerMicrosecond) }
    static func microseconds(_ ticks: UInt64) -> Double { Double(ticks) / ticksPerMicrosecond }

    static func start(_ qos: QualityOfService, _ group: DispatchGroup, _ body: @escaping () -> Void) {
        group.enter()
        let thread = Thread {
            body()
            group.leave()
        }
        thread.qualityOfService = qos
        thread.start()
    }

    /// `count` spinners at `qos` for as long as `body` runs; each one's account.
    static func spinning(_ count: Int, _ qos: QualityOfService, during body: () -> Void) -> [switch_sched_spin] {
        let stop = UnsafeMutablePointer<Bool>.allocate(capacity: 1)
        let accounts = UnsafeMutablePointer<switch_sched_spin>.allocate(capacity: max(count, 1))
        defer {
            stop.deallocate()
            accounts.deallocate()
        }
        stop.initialize(to: false)
        accounts.initialize(repeating: switch_sched_spin(), count: max(count, 1))
        let group = DispatchGroup()
        for index in 0..<count {
            start(qos, group) { switch_sched_spin(stop, accounts + index) }
        }
        body()
        stop.pointee = true
        group.wait()
        return Array(UnsafeBufferPointer(start: accounts, count: count))
    }

    /// Deadline wakes on a realtime thread (no coalescing slack): microseconds late, and whether it was realtime.
    static func timerWakes(periodMicroseconds: Double, count: Int) -> ([Double], Bool) {
        let late = UnsafeMutablePointer<UInt64>.allocate(capacity: count)
        let realtime = UnsafeMutablePointer<Bool>.allocate(capacity: 1)
        defer {
            late.deallocate()
            realtime.deallocate()
        }
        let period = ticks(microseconds: periodMicroseconds)
        let group = DispatchGroup()
        start(.userInteractive, group) {
            realtime.pointee = switch_sched_make_realtime(period, period / 10, period / 2)
            switch_sched_timer_wakes(period, UInt32(count), late)
        }
        group.wait()
        return ((0..<count).map { microseconds(late[$0]) }, realtime.pointee)
    }

    /// A user-interactive waiter woken by a utility-class signaller: microseconds from signal to running.
    static func handoffs(count: Int, gapMicroseconds: Double) -> [Double] {
        guard let handoff = switch_sched_handoff_create(UInt32(count)) else { return [] }
        let latency = UnsafeMutablePointer<UInt64>.allocate(capacity: count)
        defer {
            latency.deallocate()
            switch_sched_handoff_destroy(handoff)
        }
        let gap = ticks(microseconds: gapMicroseconds)
        let group = DispatchGroup()
        start(.userInteractive, group) { switch_sched_handoff_wait(handoff, latency) }
        start(.utility, group) { switch_sched_handoff_signal(handoff, gap) }
        group.wait()
        return (0..<count).map { microseconds(latency[$0]) }
    }
}

/// Whether the kernel preempts and time-slices on this hardware: a high-priority thread woken while every core runs
/// lower-priority spinners must run within a quantum (10 ms), by a timer or by another thread's signal, and spinners of
/// one class sharing too few cores must each get their turn. Without the timer interrupt at quantum expiry or the IPI
/// that preempts a core, these waits become a quantum or more and a spinner can starve.
struct CPUSchedTest: HWTest {
    let id = "cpu.sched"
    let name = "Preemption and time slicing under all-core load"
    let subsystem = Subsystem.cpu
    var timeout: TimeInterval { 60 }

    static let quantumMicroseconds = 10_000.0

    func run(_ context: TestContext) async throws -> Outcome {
        let cores = ProcessInfo.processInfo.activeProcessorCount
        var metrics = Metrics()
        metrics.set("cores", cores)

        var timer: [Double] = []
        var realtime = false
        _ = SchedProbe.spinning(cores, .utility) {
            (timer, realtime) = SchedProbe.timerWakes(periodMicroseconds: 2_000, count: 200)
        }
        var handoff: [Double] = []
        _ = SchedProbe.spinning(cores - 1, .utility) {
            handoff = SchedProbe.handoffs(count: 200, gapMicroseconds: 1_000)
        }
        let slices = SchedProbe.spinning(cores + 1, .utility) { Thread.sleep(forTimeInterval: 2) }
        let (idle, idleRealtime) = SchedProbe.timerWakes(periodMicroseconds: 30_000, count: 25)

        metrics.set("realtime", realtime && idleRealtime)
        for (label, values) in [("timer", timer), ("handoff", handoff), ("idle", idle)] {
            metrics.set("\(label)_p50_us", Stats.percentile(values, 0.5), digits: 0)
            metrics.set("\(label)_p99_us", Stats.percentile(values, 0.99), digits: 0)
            metrics.set("\(label)_max_us", values.max() ?? 0, digits: 0)
        }
        let passes = slices.map { Double($0.passes) }
        let fair = passes.reduce(0, +) / Double(max(passes.count, 1))
        let leastShare = fair > 0 ? (passes.min() ?? 0) / fair : 0
        let longestStall = slices.map { SchedProbe.microseconds($0.longest_stall) }.max() ?? 0
        metrics.set("slice_least_share", leastShare, digits: 2)
        metrics.set("slice_longest_stall_us", longestStall, digits: 0)

        let quantum = Self.quantumMicroseconds
        // A timeshare waiter's 30 ms sleeps may be coalesced up to 1 ms late by design (arm_timer.c tcoal ts max).
        let idleBound = idleRealtime ? 1_000.0 : 2_000.0
        return .judged(
            [
                ("the waiter ran as a realtime thread", realtime && idleRealtime),
                ("timer wakes under load: median under 1 ms", Stats.percentile(timer, 0.5) < 1_000),
                ("timer wakes under load: p99 within a quantum", Stats.percentile(timer, 0.99) < quantum),
                ("signalled wakes under load: median under 1 ms", Stats.percentile(handoff, 0.5) < 1_000),
                ("signalled wakes under load: p99 within a quantum", Stats.percentile(handoff, 0.99) < quantum),
                ("every spinner got at least half its share", leastShare >= 0.5),
                ("no spinner waited more than 20 quanta", longestStall < 20 * quantum),
                (
                    "timer wakes from idle: p99 under 1 ms past the coalescing slack",
                    Stats.percentile(idle, 0.99) < idleBound
                ),
            ], metrics)
    }
}
