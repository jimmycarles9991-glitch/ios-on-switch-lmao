import Darwin
import HWTestCore

/// SOC_THERM's hardware throttle as the kernel programmed it (kern.switch_soctherm, debug kernels;
/// drivers/SwitchPower/SwitchSocThermThrottle.h): L4T's T210B01 setting is one heavy indicator at PLLX 94.5 C that
/// slows neither the CPU (256/256) nor the GPU (vector none), and no light or OC throttle. gpu_depth is the GPU throttle
/// SOC_THERM drives now; anything but 0 would slow the GPC clock by up to 8.
struct SocThermThrottleTest: HWTest {
    let id = "system.soctherm_throttle"
    let name = "SOC_THERM hardware throttle as L4T sets it, GPU never throttled"
    let subsystem = Subsystem.system

    static let sysctl = "kern.switch_soctherm"
    static let reported = [
        "heavy_pllx_c", "heavy_req", "heavy_cpu", "heavy_gpu", "heavy_prio", "lock", "global", "light", "oc_throttle",
        "cdiv_therm", "gpu_depth", "cpu_skip", "throt_status", "heavy_ups", "oc_events",
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let read = KernelSysctl.string(Self.sysctl)
        guard case .value(let line) = read else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(read.error))
            return .skip(KernelSysctl.unreadable(Self.sysctl, read.error), metrics)
        }
        guard line != "down" else { return .fail("SOC_THERM is not up (\(Self.sysctl) reads down)") }
        let fields = Self.fields(line)
        var metrics = Metrics()
        for key in Self.reported { metrics.set(key, fields[key] ?? "-") }
        return .judged(
            [
                (
                    "heavy at PLLX 94.5 C, requesting CPU and GPU (L4T's LEVEL2 throttrip)",
                    fields["heavy_pllx_c"] == "94.5" && fields["heavy_req"] == "cpu+gpu"
                ),
                (
                    "heavy slows neither: CPU 256/256, GPU none, priority 100",
                    fields["heavy_cpu"] == "256/256" && fields["heavy_gpu"] == "none" && fields["heavy_prio"] == "100"
                ),
                ("no light or OC throttle", fields["light"] == "off" && fields["oc_throttle"] == "0x0"),
                (
                    "throttle enabled with the CPU divider under it",
                    fields["global"] == "on" && fields["cdiv_therm"] == "on"
                ),
                ("SOC_THERM drives no GPU throttle now", fields["gpu_depth"] == "0"),
            ], metrics)
    }

    static func fields(_ line: String) -> [String: String] {
        var fields: [String: String] = [:]
        for token in line.split(separator: " ") {
            let pair = token.split(separator: "=", maxSplits: 1)
            if pair.count == 2 { fields[String(pair[0])] = String(pair[1]) }
        }
        return fields
    }
}
