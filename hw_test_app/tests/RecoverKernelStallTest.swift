import Foundation
import HWTestCore
import HWTestGPU
import Metal
import QuartzCore
import UIKit

/// A kernel channel stuck again before its redone work landed, while a client's context is resident (debug kernels,
/// drivers/TegraGM20B/TegraGM20BCopyResetABI.h ArmAgain): the kernel cuts the present's copy as a context-switch timeout
/// tick on the present's channel would, then cuts its redo too, while this app keeps GR busy with long draws. The second
/// cut kills the client contexts the GPU holds, resets the copy engine and copies the frame again; the display flips it
/// and the compositor's frames keep coming. On T-v4 the second cut panicked the system.
struct RecoverKernelStallTest: HWTest {
    let id = "gpu.recover.kernel_second_timeout"
    let name = "The present stuck again before its redo landed: resident clients die, the frame is redone, no panic"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let armAgain: UInt32 = 3
    /// enum tgm_ctxsw's TGM_CTXSW_REDO and TGM_CTXSW_FATAL
    static let redo: Int32 = 3, fatal: Int32 = 5
    static let loadBuffers = 24, drawsPerBuffer = 3
    static let gapBoundMs = 600.0

    func run(_ context: TestContext) async throws -> Outcome {
        guard let connection = GPUConnection() else { return .fail("no TegraGM20B connection") }
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        let probeBench: BenchRunner
        do {
            probeBench = try BenchRunner.open()
        } catch {
            return .fail("the compositor probe's bench did not open: \(error)")
        }
        let bands = try gpu.render(vertex: "bands_vertex", fragment: "bands_blend_fragment", format: .bgra8Unorm)
        let target = try gpu.texture(.bgra8Unorm, 1280, 720)
        let compositor = await context.onStage { stage -> RecoverCompositorProbe in
            let probe = RecoverCompositorProbe(device: gpu.device, bench: probeBench)
            probe.probe.frame = CGRect(x: stage.bounds.width - 58, y: 10, width: 48, height: 48)
            probe.probe.contentsScale = UIScreen.main.scale
            stage.layer.addSublayer(probe.probe)
            return probe
        }
        await MainActor.run { compositor.start() }
        try await Task.sleep(nanoseconds: 500_000_000)
        await MainActor.run { compositor.measure() }
        defer { _ = PresentCopyResetTest.call(connection, PresentCopyResetTest.disarm) }

        let load = try (0..<Self.loadBuffers).map { _ in try Self.bands(gpu, bands, target) }
        let (kr, _) = PresentCopyResetTest.call(connection, Self.armAgain)
        var reply: [UInt8] = []
        if kr == KERN_SUCCESS { reply = await PresentCopyResetTest.settle(connection, {}) }
        load.forEach { $0.waitUntilCompleted() }
        await MainActor.run { compositor.stop() }
        let presented = await MainActor.run { compositor.presented }
        let fillErrors = await MainActor.run { compositor.fillErrors }
        await context.clearStage()
        if kr == GPUConnection.kIOReturnUnsupported { return .skip("a release kernel has no copy reset") }
        guard kr == KERN_SUCCESS else { return .fail("arm: \(GPUConnection.hex(kr))") }

        let state = GPUConnection.int32(reply, 0)
        let status = GPUConnection.int32(reply, 4)
        let redone = Int(GPUConnection.uint32(reply, 12))
        let killed = Int(GPUConnection.uint32(reply, 16))
        let verdict = GPUConnection.int32(reply, 20)
        let shown = Pacing(times: presented.sorted(), period: 1.0 / 60)
        let gapMs = shown.intervals.max() ?? 0
        var metrics = Metrics()
        metrics.set("state", Int(state))
        metrics.set("status", Int(status))
        metrics.set("redone", redone)
        metrics.set("killed", killed)
        metrics.set("second_verdict", Int(verdict))
        metrics.set("engine_status", String(format: "%08x", GPUConnection.uint32(reply, 8)))
        metrics.set("load_failed", load.filter { $0.status == .error }.count)
        metrics.set("present_gap_ms", gapMs, digits: 1)
        metrics.set("gap_bound_ms", Self.gapBoundMs, digits: 0)
        metrics.set("present_frames", shown.frames)
        metrics.set("probe_fill_errors", fillErrors)
        if state == PresentCopyResetTest.armed {
            return .skip("no frame came through a GPU copy (direct scanout)", metrics)
        }
        return .judged(
            [
                ("the copy engine was reset under the present and under its redo", state == PresentCopyResetTest.done),
                ("the second cut reset and redid the work, never the hardware-failure row", verdict == Self.redo),
                ("the frame was copied again on the GPU", redone >= 2),
                ("the display flipped it: status 0, no CPU copy", status == 0),
                ("the compositor kept presenting within the bound", shown.frames > 0 && gapMs <= Self.gapBoundMs),
            ], metrics)
    }

    /// One command buffer of full-screen draws, each over ten milliseconds of GR's work (DrawBands.metal), committed
    static func bands(_ gpu: GPU, _ pipeline: MTLRenderPipelineState, _ target: MTLTexture) throws -> MTLCommandBuffer {
        try gpu.commit { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            for _ in 0..<drawsPerBuffer { encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3) }
            encoder.endEncoding()
        }
    }
}
