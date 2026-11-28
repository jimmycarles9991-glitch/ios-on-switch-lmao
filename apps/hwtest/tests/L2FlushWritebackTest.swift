import Foundation
import HWTestCore
import HWTestKit

/// Whether the host's L2_FLUSH_DIRTY writes the GPU's L2 back on T210, in the kernel's MEM_OP_C/D form and in
/// deko3d's MEM_OP_B form (core/L2Writeback.swift). Every round, on the bench's raw device: the CPU fills a 64x64
/// RGBA32Float write-combined pitch target under a lease (whose release drops the L2's lines of it at the job's start),
/// the 3D engine draws a pattern over it through the L2-cached GPU mapping, a second pass's draw carries the arm's host
/// methods, the job ends, and the CPU reads the target through its write-combined mapping (Normal non-cacheable:
/// TegraGM20BABI.h TegraGM20BBoCoherent, TegraGM20BUserClient.cpp boMapOptions) with no lease. The arms run
/// interleaved, each round's pattern its own, so a stale word names its round: no_flush and bracket are the controls,
/// mem_op_d, job_end (the kernel's own External end) and mem_op_b must read exact. mem_op_b runs after the rest and
/// stops at its first failure, since a PBDMA that rejects the method kills the channel. Another process's External job
/// flushes the same L2, so a control read exact in a round is no proof of anything; one stale round of each control
/// is what makes the flush arms' exact reads mean the MEM_OP wrote the L2 back.
struct L2FlushWritebackTest: HWTest {
    let id = "gpu.x1.l2_flush_writeback"
    let name = "L2_FLUSH_DIRTY writes the GPU's L2 back for the CPU: MEM_OP_D, the kernel's job end and MEM_OP_B"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    typealias Arm = L2Writeback.Arm

    func run(_ context: TestContext) async throws -> Outcome {
        let runner: BenchRunner
        do {
            runner = try BenchRunner.open()
        } catch {
            return .fail("the bench did not open: \(error)")
        }
        var tallies: [Arm: L2Writeback.Tally] = [:]
        var exactJobs = true
        let shared: [Arm] = [.noFlush, .bracket, .memOpD, .jobEnd]
        for round in 0 ..< L2Writeback.rounds {
            for arm in shared where tallies[arm]?.failure == nil {
                exactJobs = Self.step(runner, arm, round, &tallies) && exactJobs
            }
        }
        for round in 0 ..< L2Writeback.rounds where tallies[.memOpB]?.failure == nil {
            exactJobs = Self.step(runner, .memOpB, round, &tallies) && exactJobs
        }
        var finding = L2Writeback.judge(tallies)
        var base = Metrics()
        base.set("exact_jobs", exactJobs)
        if !exactJobs && finding.verdict == .pass {
            finding.verdict = .inconclusive
            finding.note = "the kernel takes no addressed jobs: every job end flushes"
        }
        var outcome = finding.outcome(probe: "mesa-switch-2.13", base: base, proof: runner.summary)
        if finding.verdict == .fail { outcome.verdict = .fail }
        return outcome
    }

    /// One round of `arm` into its tally; false where the kernel took no addressed jobs
    static func step(
        _ runner: BenchRunner, _ arm: Arm, _ round: Int, _ tallies: inout [Arm: L2Writeback.Tally]
    ) -> Bool {
        let pattern = L2Writeback.pattern(arm, round: round)
        let sentinel = L2Writeback.sentinel(arm, round: round)
        let read = runner.l2Round(
            arm, pattern: pattern, sentinel: sentinel, width: L2Writeback.width, height: L2Writeback.height
        )
        var tally = tallies[arm] ?? L2Writeback.Tally()
        let emitted = arm.carriesMethods ? 1 : 0
        if read.status != "ok" {
            let lost = read.lostError == 0 ? "" : " e\(read.lostError)"
            tally.failure = "round \(round) \(read.status)\(lost): \(read.why)"
        } else if read.emitted != emitted {
            tally.failure = "round \(round): its methods reached \(read.emitted) draws, not \(emitted)"
        } else {
            tally.add(L2Writeback.compare(read.words, pattern: pattern, sentinel: sentinel))
        }
        tallies[arm] = tally
        return read.status != "ok" || read.exactJobs
    }
}
