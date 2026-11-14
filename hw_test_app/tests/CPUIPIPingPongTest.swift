import Darwin
import Foundation
import HWTestCore
import SwitchIPIProbe

/// Two threads waking each other through Mach semaphores (cpu/switch_ipi_probe.c), each waker keeping its own core
/// busy for 50 us after its signal so the other thread wakes on another core: every wake is a cross-core IPI (ArmCpu's
/// SGI send and acknowledge). No wake may be lost; how long each takes is reported. The kernel's per-core IPI counts
/// for the same window are in the SwitchPower 10 s line ("; irq cpuN ... ipi").
struct CPUIPIPingPongTest: HWTest {
    let id = "cpu.ipi_pingpong"
    let name = "Cross-core wakes: two threads waking each other"
    let subsystem = Subsystem.cpu
    var timeout: TimeInterval { 30 }

    static let rounds = 5_000
    static let holdMicroseconds = 50.0
    static let quantumMicroseconds = 10_000.0

    func run(_ context: TestContext) async throws -> Outcome {
        let rounds = Self.rounds
        guard let pingpong = switch_ipi_pingpong_create(UInt32(rounds)) else {
            return .fail("Mach semaphores could not be made")
        }
        let first = UnsafeMutablePointer<UInt64>.allocate(capacity: rounds)
        let second = UnsafeMutablePointer<UInt64>.allocate(capacity: rounds)
        let lost = UnsafeMutablePointer<UInt32>.allocate(capacity: 2)
        defer {
            first.deallocate()
            second.deallocate()
            lost.deallocate()
            switch_ipi_pingpong_destroy(pingpong)
        }
        first.initialize(repeating: 0, count: rounds)
        second.initialize(repeating: 0, count: rounds)
        lost.initialize(repeating: 0, count: 2)
        let hold = SchedProbe.ticks(microseconds: Self.holdMicroseconds)
        let limit = SchedProbe.ticks(microseconds: 1_000_000)
        let group = DispatchGroup()
        SchedProbe.start(.userInteractive, group) {
            lost[0] = switch_ipi_pingpong_side(pingpong, true, hold, limit, first)
        }
        SchedProbe.start(.userInteractive, group) {
            lost[1] = switch_ipi_pingpong_side(pingpong, false, hold, limit, second)
        }
        group.wait()

        let wakes = (0..<rounds).flatMap { [first[$0], second[$0]] }.filter { $0 != 0 }.map(SchedProbe.microseconds)
        let lostWakes = Int(lost[0]) + Int(lost[1])
        var metrics = Metrics()
        metrics.set("rounds", rounds)
        metrics.set("wakes", wakes.count)
        metrics.set("lost", lostWakes)
        metrics.set("wake_p50_us", Stats.percentile(wakes, 0.5), digits: 1)
        metrics.set("wake_p99_us", Stats.percentile(wakes, 0.99), digits: 1)
        metrics.set("wake_max_us", wakes.max() ?? 0, digits: 1)
        return .judged(
            [
                ("every wake arrived (none waited 1 s)", lostWakes == 0 && wakes.count == 2 * rounds),
                ("cross-core wakes: median under 1 ms", Stats.percentile(wakes, 0.5) < 1_000),
                ("cross-core wakes: p99 within a quantum", Stats.percentile(wakes, 0.99) < Self.quantumMicroseconds),
            ], metrics)
    }
}
