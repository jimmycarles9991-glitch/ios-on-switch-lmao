import Darwin
import HWTestCore

/// RAMFC timeslices as the host keeps them (debug.switch_gpu_ramfc, debug kernels). A level change's timeslice is
/// written only after a preempt of the channel has completed, so the host's save of a channel it still holds cannot land
/// on top of it; a write that raced that save reads back as the old value. The kernel changes its probe channel's
/// timeslice through that path and back, each read after a job on the channel, and reads every live client channel's
/// against the value last written to it once it has completed a job since.
struct RamfcTimesliceTest: HWTest {
    let id = "gpu.ramfc_timeslice"
    let name = "RAMFC timeslice after a level change: a change and back, and every client channel"
    let subsystem = Subsystem.metal

    static let sysctl = "debug.switch_gpu_ramfc"

    func run(_ context: TestContext) async throws -> Outcome {
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
        let fields = DcHost1xClassTest.fields(line)
        var metrics = Metrics()
        let keys = [
            "channels", "checked", "high", "mismatched", "first_chid", "first_ramfc", "first_expected", "flip_chid",
            "flip_from", "flip_to", "flip_read", "back_read", "flip_ret",
        ]
        for key in keys {
            if let value = fields[key] { metrics.set(key, value) }
        }
        return .judged(
            [
                ("the probe channel's change and change back both ran (flip_ret = 0)", fields["flip_ret"] == "0"),
                (
                    "the changed timeslice held through a job (flip_read = flip_to)",
                    fields["flip_read"] != nil && fields["flip_read"] == fields["flip_to"]
                ),
                (
                    "the timeslice changed back held through a job (back_read = flip_from)",
                    fields["back_read"] != nil && fields["back_read"] == fields["flip_from"]
                ),
                ("every checked client channel reads its last write (mismatched = 0)", fields["mismatched"] == "0"),
            ], metrics)
    }
}
