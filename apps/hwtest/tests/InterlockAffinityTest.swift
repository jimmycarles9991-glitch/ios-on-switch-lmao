import Foundation
import HWTestCore
import HWTestKit
import SwitchBench

/// Whether every SR_AFFINITY byte a pixel program sees fits the interlock's ticket buffer. The pixel interlock puts a
/// warp's ticket at word (SR_AFFINITY & 0xff) * 256 + (ticket & 0xff), as NVIDIA's own programs do, and the driver
/// sizes the buffer (4 KiB when this was written: affinity bytes 0..3) with nothing on the device bounding the byte.
/// A byte past the buffer's end spins on and adds into whatever is mapped after it, with no fault. Each pixel of a
/// 1280x720 draw on the bench's raw device stores SR_AFFINITY (S2R 0x1c) beside a second register: SR_AFFINITY again,
/// then SR_VIRTID (S2R 0x03), whose SM and warp fields are the control. The control holds when SR_VIRTID shows as many
/// SM ids as GetInfo counts TPCs (one SM each on GM20B) and more than one warp: only S2R reading per-warp hardware
/// state into the target can give that. The test FAILS when a byte overflows (naming it and the driver's
/// allocation), and when the control does not hold, as nothing then shows S2R's value reached the target. The
/// affinity byte itself is expected constant: GM20B has one GPC, and BENCH3's state 357 read byte 0 in every pixel
/// (learn/maxwell-ext.md).
struct InterlockAffinityTest: HWTest {
    let id = "gpu.x1.interlock_affinity"
    let name = "SR_AFFINITY on both SMMs: every byte fits the pixel interlock's ticket buffer"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let runner: BenchRunner
        do {
            runner = try BenchRunner.open()
        } catch {
            return .fail("the bench did not open: \(error)")
        }
        var gpcs: UInt32 = 0, tpcs: UInt32 = 0
        let unitsRead = switch_bench_gpu_units(&gpcs, &tpcs) == 0
        let experiments = try InterlockAffinity.experiments()
        let readings = experiments.map { experiment -> Result<InterlockAffinity.Reading, TestFailure> in
            let outcome = runner.run(experiment)
            guard outcome.status == "ok", let texels = outcome.texels else {
                let lost = outcome.lostError == 0 ? "" : " e\(outcome.lostError)"
                return .failure(TestFailure("\(outcome.status)\(lost): \(outcome.why)"))
            }
            return .success(InterlockAffinity.read(texels))
        }
        let finding = InterlockAffinity.judge(
            screen: readings[0], control: readings[1], tpcs: unitsRead ? Int(tpcs) : 0, ticketBytes: runner.ticketBytes
        )
        var base = Metrics()
        base.set("gpcs", unitsRead ? Int(gpcs) : -1)
        var outcome = finding.outcome(probe: "mesa-switch-2.2", base: base, proof: runner.summary)
        if finding.verdict != .pass { outcome.verdict = .fail }
        return outcome
    }
}
