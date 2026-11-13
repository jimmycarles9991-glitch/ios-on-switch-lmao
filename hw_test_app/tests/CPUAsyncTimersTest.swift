import Darwin
import Foundation
import HWTestCore

struct CPUAsyncTimersTest: HWTest {
    let id = "cpu.async_timers"
    let name = "Swift and dispatch deadline clocks"
    let subsystem = Subsystem.cpu
    var timeout: TimeInterval { 12 }

    private enum ClockCase: String, CaseIterable {
        case relative, continuous, suspending
    }

    private func elapsed(_ start: UInt64, _ base: mach_timebase_info_data_t) -> Double {
        Double(mach_absolute_time() - start) * Double(base.numer) / Double(base.denom) / 1e6
    }

    func run(_ context: TestContext) async throws -> Outcome {
        var base = mach_timebase_info_data_t()
        guard mach_timebase_info(&base) == KERN_SUCCESS, base.numer != 0, base.denom != 0 else {
            return .fail("mach_timebase_info returned an invalid ratio")
        }
        var metrics = Metrics()
        metrics.set("timebase_numer", Int(base.numer))
        metrics.set("timebase_denom", Int(base.denom))
        metrics.set("uptime_s", Double(mach_continuous_time()) * Double(base.numer) / Double(base.denom) / 1e9)
        var checks: [(String, Bool)] = []

        let dispatch = Waiter<Double>()
        let start = mach_absolute_time()
        DispatchQueue.global(qos: .userInitiated).asyncAfter(deadline: .now() + 0.05) {
            dispatch.resume(elapsed(start, base))
        }
        let dispatchMS = await dispatch.wait(timeout: 2)
        metrics.set("dispatch_ms", dispatchMS ?? -1)
        checks.append(("dispatch 50 ms deadline completes within 500 ms", (40...500).contains(dispatchMS ?? -1)))

        for clock in ClockCase.allCases {
            let waiter = Waiter<Double>()
            let task = Task {
                let start = mach_absolute_time()
                do {
                    switch clock {
                    case .relative:
                        try await Task.sleep(nanoseconds: 50_000_000)
                    case .continuous:
                        let timer = ContinuousClock()
                        try await timer.sleep(until: timer.now.advanced(by: .milliseconds(50)), tolerance: .zero)
                    case .suspending:
                        let timer = SuspendingClock()
                        try await timer.sleep(until: timer.now.advanced(by: .milliseconds(50)), tolerance: .zero)
                    }
                    waiter.resume(elapsed(start, base))
                } catch {
                    waiter.resume(-1)
                }
            }
            // The independent dispatch deadline also bounds a broken Swift clock conversion.
            let milliseconds = await waiter.wait(timeout: 2)
            task.cancel()
            metrics.set("\(clock.rawValue)_ms", milliseconds ?? -1)
            checks.append(
                ("\(clock.rawValue) 50 ms sleep completes within 500 ms", (40...500).contains(milliseconds ?? -1)))
        }

        let cancelled = Waiter<Bool>()
        let task = Task {
            do {
                try await Task.sleep(nanoseconds: 60_000_000_000)
                cancelled.resume(false)
            } catch is CancellationError {
                cancelled.resume(true)
            } catch {
                cancelled.resume(false)
            }
        }
        DispatchQueue.global(qos: .userInitiated).asyncAfter(deadline: .now() + 0.05) { task.cancel() }
        let cancellation = await cancelled.wait(timeout: 2) == true
        task.cancel()
        metrics.set("cancel_resumed", cancellation)
        checks.append(("cancelling a pending Swift sleep resumes with CancellationError", cancellation))
        return .judged(checks, metrics)
    }
}
