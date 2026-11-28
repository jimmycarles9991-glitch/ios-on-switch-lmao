import Darwin
import HWTestCore
import HWTestKit
import HWTestShared

/// The DFLL's rows asked from above and from below (drivers/SwitchPower/SwitchThermalDfllLanding.h, debug kernels):
/// the kernel asks each case's first rate, lets it settle, asks the row and measures the CPU's clock where it runs
/// 2, 10, 50 and 200 ms later, with the loop's registers beside each reading and the SoC's temperature at the start.
/// The run lands DfllLanding.landings times, one after another, and DfllLanding judges every landing's 200 ms readings;
/// the earlier ones are the settling, kept in the report.
struct CPUDfllLandingTest: HWTest {
    let id = "cpu.dfll_landing"
    let name = "DFLL rows land on the rate asked, from above and from below, in each of five landings"
    let subsystem = Subsystem.cpu

    static let sysctl = "debug.switch_perf_dfll_landing"

    func run(_ context: TestContext) async throws -> Outcome {
        var landings: [[String: String]] = []
        for _ in 0 ..< DfllLanding.landings {
            var run: Int32 = 1
            guard sysctlbyname(Self.sysctl, nil, nil, &run, MemoryLayout<Int32>.size) == 0 else {
                let error = errno
                var metrics = Metrics()
                metrics.set("sysctl", KernelSysctl.code(error))
                return .skip(KernelSysctl.unreadable(Self.sysctl, error), metrics)
            }
            guard case .value(let line) = KernelSysctl.string(Self.sysctl) else {
                return .skip(KernelSysctl.unreadable(Self.sysctl, errno), Metrics())
            }
            landings.append(KernelLine.fields(line))
        }
        let (checks, metrics) = DfllLanding.judge(landings: landings)
        return .judged(checks, metrics)
    }
}
