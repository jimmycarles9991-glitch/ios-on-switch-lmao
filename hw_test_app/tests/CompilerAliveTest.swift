import Darwin
import HWTestCore
import SwitchCompilerProbe

/// The system shader compiler at the end of the run: the boot's first compiler still serves (the GPU kext's broker,
/// kern.switch_shader_compiler: generation 1, nothing lost), in the jetsam band and with the limits its launcher gives
/// it (SwitchShaderCompilerJetsam.h: launchd's jetsam properties overlay on iOS 27, the BlueTool stand-in's spawn on
/// 17), read back from the kernel with memorystatus_control. Boot 12 of T-v3e had launchd's daemon default instead:
/// band 0 once aged, InactiveHard 6 MB, and seven compilers jetsam-killed in two minutes.
struct CompilerAliveTest: HWTest {
    let id = "system.compiler_alive"
    let name = "The shader compiler runs the whole boot in its own jetsam band and limits"
    let subsystem = Subsystem.system

    func run(_ context: TestContext) async throws -> Outcome {
        var probe = switch_compiler_probe()
        switch_compiler_probe(&probe)
        let band = Int(probe.wanted_band)
        let active = Int(probe.wanted_active_mb), inactive = Int(probe.wanted_inactive_mb)

        var metrics = Metrics()
        metrics.set("pid", Int(probe.pid))
        metrics.set("generation", Int(probe.generation))
        metrics.set("losses", Int(probe.losses))
        metrics.set("band", Int(probe.band))
        metrics.set("limit_mb", Int(probe.limit_mb))
        metrics.set("active_mb", Int(probe.active_mb))
        metrics.set("active_fatal", Int(probe.active_fatal))
        metrics.set("inactive_mb", Int(probe.inactive_mb))
        metrics.set("inactive_fatal", Int(probe.inactive_fatal))
        metrics.set("state", "0x" + String(probe.state, radix: 16))
        metrics.set("line_error", Self.code(probe.line_error))
        metrics.set("priority_error", Self.code(probe.priority_error))
        metrics.set("limits_error", Self.code(probe.limits_error))

        let deliberate = DeliberateCompilerDeaths.shared.count
        let read = probe.line_error == 0
        let asked = read && probe.pid > 0
        return .judged(
            [
                (
                    read
                        ? "the kernel's broker line read (kern.switch_shader_compiler)"
                        : KernelSysctl.unreadable("kern.switch_shader_compiler", probe.line_error), read
                ),
                ("a compiler is registered", probe.pid > 0),
                (
                    "no compiler lost this boot (losses \(deliberate): gpu.compile.first_draw's on purpose)",
                    Int(probe.losses) == deliberate
                ),
                (
                    "the boot's first registration still serves (generation \(1 + deliberate); more with no loss: a "
                        + "full store registered again)",
                    Int(probe.generation) == 1 + deliberate
                ),
                (
                    Self.answered("its jetsam band", probe.priority_error),
                    asked && probe.priority_error == 0
                ),
                ("in band \(band)", Int(probe.band) == band),
                ("held to \(active) MB now", Int(probe.limit_mb) == active),
                ("never clean or idle-exited (not tracked)", probe.state & Self.trackedOrIdleExit == 0),
                (
                    Self.answered("its memory limits", probe.limits_error),
                    asked && probe.limits_error == 0
                ),
                ("active limit \(active) MB, soft", Int(probe.active_mb) == active && probe.active_fatal == 0),
                (
                    "inactive limit \(inactive) MB, hard",
                    Int(probe.inactive_mb) == inactive && probe.inactive_fatal != 0
                ),
            ], metrics, proof: "pid \(probe.pid)")
    }

    /// xnu bsd/sys/kern_memorystatus.h kMemorystatusTracked | kMemorystatusSupportsIdleExit, on 10063 and 12377 alike
    static let trackedOrIdleExit: UInt32 = 0x08 | 0x10

    static func code(_ error: Int32) -> String {
        error == 0 ? "none" : KernelSysctl.code(error)
    }

    /// EPERM is the app's: its entitlement (com.apple.private.memorystatus) or its sandbox refusing the call.
    static func answered(_ what: String, _ error: Int32) -> String {
        switch error {
        case 0: return "memorystatus_control answers \(what)"
        case EPERM: return "memorystatus_control refuses the app \(what) (EPERM: entitlement or sandbox)"
        case ESRCH: return "memorystatus_control knows no such pid for \(what) (ESRCH)"
        default: return "memorystatus_control fails \(what) (errno \(error))"
        }
    }
}
