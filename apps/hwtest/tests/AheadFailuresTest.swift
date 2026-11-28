import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUModels
import HWTestKit
import Metal

/// The submit worker's encode ahead of command buffers that fail (gpu/AheadFrames.swift: 15 passes a millisecond
/// apart), judged by the rule facade_ahead_failures_test.cpp pins on the host: Apple's driver stops the process on
/// both failures (gpu.gm20b.ended_encoder), so no Apple value exists; the valid frames' model is an M4 Max's draw of them
/// (0 pixels off in five runs). In order: warm, a valid frame, encoded ahead (pipelined at least 10 of its 15 encoders)
/// and drawing the model; commit_fails, whose last pass draws again after a blit encoder began: its commit fails it
/// InvalidResource (code 9, error:none) with no veto recorded; after_commit, a valid frame the worker still encodes
/// ahead and that draws the model; record_fails, whose pass 9 ends twice: it fails as it records (code 9, error:none,
/// veto bit 7, refused) and the worker is handed nothing from that failure to its commit (record_fails_handoffs 0);
/// after_record, as after_commit. failed_cbs none x2, no call skipped. Per frame <frame>_status, _pipelined, _off
/// (pixels more than a code from the model); the failures' _code, _veto, _given_back.
struct AheadFailuresTest: HWTest {
    let id = "gpu.gm20b.ahead_failures"
    let name = "A command buffer that fails while encoded ahead gives the device back and the next is encoded ahead"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    static let leastPipelined = 10
    static let vetoRefused = 1 << 7, invalidResource = 9

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        guard before["pipeline_handoffs"] != nil else {
            return before["gm20b"] == nil
                ? .skip("no pipelining counters: a release runtime")
                : .fail("a debug runtime without pipelining counters")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            let frames = try AheadFrames(gpu)
            let warm = try await frames.run(0, .none)
            checks += Self.valid("warm", warm, &metrics)
            let commitFails = try await frames.run(1, .drawInBlit)
            checks += Self.failed("commit_fails", commitFails, &metrics)
            checks.append(("commit_fails: no veto recorded", commitFails.pipelined["veto"] == 0))
            checks += Self.valid("after_commit", try await frames.run(2, .none), &metrics)
            let recordFails = try await frames.run(3, .endedTwice(at: 9))
            checks += Self.failed("record_fails", recordFails, &metrics)
            let veto = recordFails.pipelined["veto"] ?? 0
            checks.append(("record_fails: veto \(veto) has refused (bit 7)", veto & Self.vetoRefused != 0))
            metrics.set("record_fails_handoffs", recordFails.handoffsAfterFailure)
            checks.append(
                ("record_fails: \(recordFails.handoffsAfterFailure) hand-offs after the failure",
                 recordFails.handoffsAfterFailure == 0)
            )
            checks += Self.valid("after_record", try await frames.run(4, .none), &metrics)
        } catch {
            checks.append(("\(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        checks.append(
            FailedCounts.judged(before: before, after: counted, expecting: ["none": 2], own: gpu.paths, &metrics)
        )
        checks.append(FailedCounts(before: before, after: counted).skipCheck())
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// A frame that must complete on raw, encoded ahead, with the model's pixels
    static func valid(_ arm: String, _ r: AheadFrames.Reading, _ metrics: inout Metrics) -> [(String, Bool)] {
        let pipelined = r.pipelined["pipelined"] ?? -1, encoders = r.pipelined["encoders"] ?? -1
        metrics.set(arm + "_status", r.status)
        metrics.set(arm + "_pipelined", pipelined)
        metrics.set(arm + "_encoders", encoders)
        metrics.set(arm + "_off", r.pixelsOff)
        return [
            ("\(arm): \(r.status) \(r.path)", r.status == "completed" && r.path == "gm20b"),
            ("\(arm): \(pipelined) of \(encoders) encoders encoded ahead", pipelined >= leastPipelined),
            ("\(arm): \(r.pixelsOff) pixels off the model", r.status == "completed" && r.pixelsOff == 0),
        ]
    }

    /// A frame that must fail InvalidResource with no predicate
    static func failed(_ arm: String, _ r: AheadFrames.Reading, _ metrics: inout Metrics) -> [(String, Bool)] {
        metrics.set(arm + "_status", r.status)
        metrics.set(arm + "_code", r.code)
        metrics.set(arm + "_veto", r.pipelined["veto"] ?? -1)
        metrics.set(arm + "_given_back", r.pipelined["given_back"] ?? -1)
        metrics.set(arm + "_abandons", r.pipelined["abandons"] ?? -1)
        return [
            ("\(arm): \(r.status) code \(r.code) \(r.path), not error 9 error:none",
             r.status == "error" && r.code == invalidResource && r.path == "error:none"),
        ]
    }
}
