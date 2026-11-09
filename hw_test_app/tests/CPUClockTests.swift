import Darwin
import Foundation
import HWTestCore
import SwitchCPUClock

/// Every core's clock at once: each thread runs dependent ADDs (cpu/switch_cpu_clock.c) and divides the cycles by
/// its own CPU time, so a thread the scheduler preempted still reads the clock of the core it ran on.
enum CPUClock {
    /// About a quarter of a second at the A57's 1785 MHz.
    static let blocksPerSlice: UInt64 = 450_000

    static func megahertz(blocks: UInt64) -> Double {
        let start = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)
        _ = switch_cpu_add_chain(blocks)
        let nanoseconds = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - start
        return nanoseconds > 0 ? Double(blocks) * Double(SWITCH_CPU_CLOCK_BLOCK) / Double(nanoseconds) * 1000 : 0
    }

    /// One thread per core for `slices` slices: each slice's clock, [slice][thread].
    static func allCores(slices: Int) -> [[Double]] {
        let threads = ProcessInfo.processInfo.activeProcessorCount
        var clocks = [[Double]](repeating: [Double](repeating: 0, count: threads), count: slices)
        let lock = NSLock()
        DispatchQueue.concurrentPerform(iterations: threads) { thread in
            for slice in 0..<slices {
                let clock = megahertz(blocks: blocksPerSlice)
                lock.lock()
                clocks[slice][thread] = clock
                lock.unlock()
            }
        }
        return clocks
    }

    static let thermal = ["nominal", "fair", "serious", "critical"]
}

struct CPUClockTest: HWTest {
    let id = "cpu.clock"
    let name = "Every core's clock under load"
    let subsystem = Subsystem.cpu

    func run(_ context: TestContext) async throws -> Outcome {
        let slices = CPUClock.allCores(slices: 4)
        let cores = slices.last!.indices.map { thread in slices.dropFirst().map { $0[thread] }.max() ?? 0 }
        var metrics = Metrics()
        metrics.set("cores", cores.count)
        for (thread, clock) in cores.enumerated() { metrics.set("mhz\(thread)", clock, digits: 0) }
        let fastest = cores.max() ?? 0
        let slowest = cores.min() ?? 0
        metrics.set("mhz_max", fastest, digits: 0)
        metrics.set("mhz_min", slowest, digits: 0)
        return .judged(
            [
                ("the fastest core clocks at least 1000 MHz", fastest >= 1000),
                ("every core within 10% of the fastest", slowest >= 0.9 * fastest),
            ], metrics)
    }
}

/// Twenty seconds of every core at full load: the clock at the end against the start, and the thermal state.
struct CPUSustainedTest: HWTest {
    let id = "cpu.sustained"
    let name = "Clock held over 20 s of all-core load (throttling)"
    let subsystem = Subsystem.cpu
    var timeout: TimeInterval { 90 }

    func run(_ context: TestContext) async throws -> Outcome {
        let before = ProcessInfo.processInfo.thermalState
        let slices = CPUClock.allCores(slices: 80).map { Stats.mean($0) }
        let after = ProcessInfo.processInfo.thermalState
        let start = Stats.mean(Array(slices.prefix(8)))
        let end = Stats.mean(Array(slices.suffix(8)))
        var metrics = Metrics()
        metrics.set("mhz_start", start, digits: 0)
        metrics.set("mhz_end", end, digits: 0)
        metrics.set("mhz_low", slices.min() ?? 0, digits: 0)
        metrics.set("throttle_pct", start > 0 ? max(0, (1 - end / start) * 100) : 0, digits: 1)
        metrics.set("thermal_start", CPUClock.thermal[before.rawValue])
        metrics.set("thermal_end", CPUClock.thermal[after.rawValue])
        return .judged(
            [
                ("the clock at the end within 10% of the start", end >= 0.9 * start),
                ("not thermally serious after it", after.rawValue < ProcessInfo.ThermalState.serious.rawValue),
            ], metrics)
    }
}
