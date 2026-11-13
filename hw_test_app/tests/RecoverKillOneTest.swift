import Foundation
import HWTestCore
import HWTestGPU
import Metal
import QuartzCore
import UIKit

/// One client's raw channel killed while other clients render: this process's channel runs a deliberately illegal
/// method in a scratch command buffer, committed right behind eight others and ahead of eight committed once it failed,
/// all on one queue. The bench's raw device (a connection and channel of its own) fills on the 2D engine throughout, read
/// back against the fill (RecoverBystander), and a probe layer another bench raw device fills shows that the compositor
/// kept presenting. Exactly the poisoned command buffer fails, the eight before
/// it keep their completion and bytes, the eight after it complete on the channel made again, the bystander sees no
/// error and no gap past the bound, one reopen, and the drivers stay ordered on the GPU (no CPU-ordered line).
struct RecoverKillOneTest: HWTest {
    let id = "gpu.recover.kill_one"
    let name = "A killed raw channel: others render on, raw comes back, only the poisoned command buffer fails"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 45 }

    static let around = 8, slice = 4096
    static let gapBoundMs = 100.0
    // MAXWELL_B SET_ALPHA_CIRCULAR_BUFFER_SIZE: GM20B traps it as a software method (L4T's nvgpu emulates it in
    // gr_gm20b_handle_sw_method); nouveau's GR interrupt reports it ILLEGAL_MTHD and the kernel kills the channel
    static let illegalMethod = 0x02dc, illegalValue = 0x800

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        let start = RawFacade.counters(of: gpu.device)
        guard start["gpu_order"] != nil else { return .skip("the facade has no recovery counters (a release build)") }
        let bystander: RecoverBystander
        let probeBench: BenchRunner
        do {
            bystander = try RecoverBystander(try BenchRunner.open())
            probeBench = try BenchRunner.open()
        } catch {
            return .fail("a bench raw device did not open: \(error)")
        }
        let compositor = await context.onStage { stage -> RecoverCompositorProbe in
            let probe = RecoverCompositorProbe(device: gpu.device, bench: probeBench)
            probe.probe.frame = CGRect(x: stage.bounds.width - 58, y: 10, width: 48, height: 48)
            probe.probe.contentsScale = UIScreen.main.scale
            stage.layer.addSublayer(probe.probe)
            return probe
        }
        bystander.start()
        await MainActor.run { compositor.start() }
        try await Task.sleep(nanoseconds: 500_000_000)
        bystander.measure()
        await MainActor.run { compositor.measure() }

        let ring = try gpu.buffer((2 * Self.around + 1) * Self.slice)
        let scratch = try gpu.texture(.bgra8Unorm, 16, 16, usage: [.shaderRead])
        let ends = CompletionTimes()
        let before = try (0..<Self.around).map { try fill(gpu, ring, slot: $0, ends) }
        let (poisoned, refusal) = try poison(gpu, ring, slot: Self.around, scratch, ends)
        let killDone = await ends.wait(before + [poisoned], seconds: 10)
        let after = try (0..<Self.around).map { try fill(gpu, ring, slot: Self.around + 1 + $0, ends) }
        let afterDone = await ends.wait(after, seconds: 10)
        try await Task.sleep(nanoseconds: 300_000_000)

        let bystanderStopped = bystander.stop(seconds: 5)
        await MainActor.run { compositor.stop() }
        let presented = await MainActor.run { compositor.presented }
        let (fillErrors, firstFillError) = await MainActor.run { (compositor.fillErrors, compositor.firstFillError) }
        await context.clearStage()
        let end = RawFacade.counters(of: gpu.device)

        let bytes = UnsafeRawBufferPointer(start: ring.contents(), count: ring.length)
        let kept = { (slots: Range<Int>) in
            slots.filter { slot in
                bytes[(slot * Self.slice)..<((slot + 1) * Self.slice)].allSatisfy { $0 == UInt8(slot + 1) }
            }.count
        }
        let beforeOK = before.filter { $0.status == .completed && $0.error == nil }.count
        let afterOK = after.filter { $0.status == .completed && $0.error == nil }.count
        let beforeKept = kept(0..<Self.around)
        let afterKept = kept((Self.around + 1)..<(2 * Self.around + 1))
        let delta = { (key: String) in (end[key] ?? 0) - (start[key] ?? 0) }
        let shown = Pacing(times: presented.sorted(), period: 1.0 / 60)
        let presentGapMs = shown.intervals.max() ?? 0
        let bystanderGapMs = Double(bystander.longestNs) / 1e6
        let recoverMs = ends.millisecondsBetween(poisoned, after.last)
        let paths = Set((before + after).map(RawFacade.path(of:)))
        let poisonedRaw = RawFacade.path(of: poisoned) == "raw"

        var metrics = Metrics()
        metrics.set("killed", poisoned.status == .error)
        metrics.set("before_ok", "\(beforeOK)/\(Self.around)")
        metrics.set("after_ok", "\(afterOK)/\(Self.around)")
        metrics.set("bystander_errors", bystander.errors)
        metrics.set("bystander_gap_ms", bystanderGapMs, digits: 1)
        metrics.set("present_gap_ms", presentGapMs, digits: 1)
        metrics.set("gap_bound_ms", Self.gapBoundMs, digits: 0)
        metrics.set("reopens", delta("reopens"))
        metrics.set("cpu_ordered", delta("cpu_ordered"))
        metrics.set("gpu_order", end["gpu_order"] ?? 0)
        metrics.set("reopen_us", end["reopen_us"] ?? 0)
        metrics.set("recover_ms", recoverMs, digits: 1)
        metrics.set("poisoned_path", RawFacade.path(of: poisoned))
        metrics.set("poisoned_error", (poisoned.error as NSError?).map { "\($0.code)" } ?? "none")
        metrics.set("before_bytes", beforeKept)
        metrics.set("after_bytes", afterKept)
        metrics.set("paths", String(paths.sorted().joined(separator: ",").prefix(80)))
        metrics.set("bystander_runs", bystander.runs)
        metrics.set("present_frames", shown.frames)
        metrics.set("probe_fill_errors", fillErrors)
        if !firstFillError.isEmpty { metrics.set("probe_fill_first", String(firstFillError.prefix(80))) }
        if !bystander.firstError.isEmpty { metrics.set("bystander_first", String(bystander.firstError.prefix(80))) }
        if let refusal { return .fail("the illegal method was refused: \(refusal)", metrics) }
        if poisoned.status == .completed && delta("reopens") == 0 {
            return .fail("the illegal method did not kill the channel: b197 0x02dc completed, no reopen", metrics)
        }
        return .judged(
            [
                ("every command buffer ended (none hung)", killDone && afterDone),
                ("the poisoned command buffer ran raw and failed", poisoned.status == .error && poisonedRaw),
                (
                    "the ones before it kept their completion and bytes",
                    beforeOK == Self.around && beforeKept == Self.around
                ),
                (
                    "the ones after it completed raw",
                    afterOK == Self.around && afterKept == Self.around && paths == ["raw"]
                ),
                ("the kernel's kill made one reopen", delta("reopens") == 1),
                ("no CPU-ordered fallback; GPU order on", delta("cpu_ordered") == 0 && end["gpu_order"] == 1),
                ("the bystander ran with no error", bystanderStopped && bystander.runs > 0 && bystander.errors == 0),
                ("the bystander never waited past the bound", bystanderGapMs <= Self.gapBoundMs),
                (
                    "the compositor kept presenting within the bound",
                    shown.frames > 0 && presentGapMs <= Self.gapBoundMs
                ),
                ("the probe's own raw device filled every frame", fillErrors == 0),
            ], metrics)
    }

    func fill(_ gpu: GPU, _ ring: MTLBuffer, slot: Int, _ ends: CompletionTimes) throws -> MTLCommandBuffer {
        guard let commands = gpu.queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder() else {
            throw TestFailure("no command buffer")
        }
        blit.fill(buffer: ring, range: (slot * Self.slice)..<((slot + 1) * Self.slice), value: UInt8(slot + 1))
        blit.endEncoding()
        ends.track(commands)
        commands.commit()
        return commands
    }

    /// A fill whose blit encoder ends with the illegal method (RawMetalProbe.mm switchRawProbeEngine:); the refusal
    /// where the facade took none
    func poison(
        _ gpu: GPU, _ ring: MTLBuffer, slot: Int, _ scratch: MTLTexture, _ ends: CompletionTimes
    ) throws
        -> (MTLCommandBuffer, String?)
    {
        guard let commands = gpu.queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder() else {
            throw TestFailure("no command buffer")
        }
        blit.fill(buffer: ring, range: (slot * Self.slice)..<((slot + 1) * Self.slice), value: UInt8(slot + 1))
        let selector = NSSelectorFromString("switchRawProbeEngine:")
        let op: NSDictionary = [
            "kind": "method", "method": Self.illegalMethod, "value": Self.illegalValue, "written": scratch,
        ]
        let refusal: String? =
            (commands as AnyObject).responds(to: selector)
            ? (commands as AnyObject).perform(selector, with: op)?.takeUnretainedValue() as? String
            : "the command buffer has no raw probe channel"
        blit.endEncoding()
        ends.track(commands)
        commands.commit()
        return (commands, refusal)
    }
}

/// When each tracked command buffer ended (its completed handler), and a wait for a set of them bounded by a timeout,
/// so a command buffer that never ends fails the test instead of hanging it
final class CompletionTimes: @unchecked Sendable {
    private let lock = NSLock()
    private var ended: [ObjectIdentifier: UInt64] = [:]
    private var waiting: (Set<ObjectIdentifier>, Waiter<Bool>)?

    func track(_ buffer: MTLCommandBuffer) {
        let key = ObjectIdentifier(buffer)
        buffer.addCompletedHandler { [self] _ in
            lock.lock()
            ended[key] = DispatchTime.now().uptimeNanoseconds
            let done = waiting.map { $0.0.isSubset(of: Set(ended.keys)) } ?? false
            let waiter = done ? waiting?.1 : nil
            lock.unlock()
            waiter?.resume(true)
        }
    }

    func wait(_ buffers: [MTLCommandBuffer], seconds: Double) async -> Bool {
        let keys = Set(buffers.map(ObjectIdentifier.init))
        let waiter = Waiter<Bool>()
        lock.lock()
        waiting = (keys, waiter)
        let done = keys.isSubset(of: Set(ended.keys))
        lock.unlock()
        if done { waiter.resume(true) }
        return await waiter.wait(timeout: seconds) == true
    }

    func millisecondsBetween(_ first: MTLCommandBuffer, _ last: MTLCommandBuffer?) -> Double {
        lock.lock()
        defer { lock.unlock() }
        guard let last, let from = ended[ObjectIdentifier(first)], let to = ended[ObjectIdentifier(last)], to >= from
        else { return 0 }
        return Double(to - from) / 1e6
    }
}
