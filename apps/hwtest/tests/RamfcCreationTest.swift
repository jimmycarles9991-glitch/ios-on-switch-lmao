import Darwin
import HWTestCore

/// A channel's first RAMFC words as the host keeps them (debug.switch_gpu_ramfc, debug kernels). nvkm puts a new channel
/// on its runlist enabled, so the host may load its RAMFC before its first submission and save that copy over a word
/// written afterwards; boot 22 read the default timeslice on the scrubber and on 4 client channels made high. The
/// kernel channels (present, scrubber) are made high and never change level, so their timeslice is the creation's.
struct RamfcCreationTest: HWTest {
    let id = "gpu.ramfc_creation"
    let name = "RAMFC timeslice written at channel creation: kernel channels and client channels"
    let subsystem = Subsystem.metal

    static let highTimeslice = "10005080"

    func run(_ context: TestContext) async throws -> Outcome {
        var run: Int32 = 1
        let sysctl = RamfcTimesliceTest.sysctl
        guard sysctlbyname(sysctl, nil, nil, &run, MemoryLayout<Int32>.size) == 0 else {
            let error = errno
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(error))
            return .skip(KernelSysctl.unreadable(sysctl, error), metrics)
        }
        guard case .value(let line) = KernelSysctl.string(sysctl) else {
            return .skip(KernelSysctl.unreadable(sysctl, errno), Metrics())
        }
        let fields = DcHost1xClassTest.fields(line)
        var metrics = Metrics()
        let keys = [
            "kernel_checked", "kernel_mismatched", "created_mismatched", "mismatched", "mismatched_owners",
            "flip_chid", "flip_from",
        ]
        for key in keys {
            if let value = fields[key] { metrics.set(key, value) }
        }
        let kernelChecked = fields["kernel_checked"].flatMap { Int($0) } ?? 0
        return .judged(
            [
                ("a kernel channel ran a job since its creation (kernel_checked > 0)", kernelChecked > 0),
                (
                    "every kernel channel holds its creation's high timeslice (kernel_mismatched = 0)",
                    fields["kernel_mismatched"] == "0"
                ),
                (
                    "the probe channel, made high, read high before its change (flip_from = 10005080)",
                    fields["flip_from"] == Self.highTimeslice
                ),
                (
                    "every client channel last written at its creation holds it (created_mismatched = 0)",
                    fields["created_mismatched"] == "0"
                ),
            ], metrics)
    }
}
