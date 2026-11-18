import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A new 4x memoryless target's first pass clears compressed memory: the raw device asks for a multisampled memoryless
/// attachment's compressed memory when the texture is made (raw_comp.cpp compPrearm), off the frame, so the pass that
/// first uses it finds it made and clears comptags alone (a ZBC value: 0.2 ms on device) instead of writing all 14.7 MB of
/// a 720x1280 4x grid into plain memory (1.3 ms, and the poster's first frames took that). Each arm makes its own
/// texture: "made" uses it after the make has had time, "immediate" right away (whether it is made by then is reported).
/// The facade's compression counters (switchRawCompression "misses": passes that took plain memory) tell which it got.
struct MsaaFirstUseTest: HWTest {
    let id = "gpu.msaa.first_use"
    let name = "A new 4x memoryless target's first pass clears compressed memory"
    let subsystem = Subsystem.metal

    static let height = 1280
    static let madeAfterMs: UInt64 = 100
    static let clearBudgetMs = 0.5

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw", !RawFacade.compression(of: gpu.device).isEmpty else {
            return .skip("the default device is not the raw facade with compression counters")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for (arm, waitMs) in [("made", Self.madeAfterMs), ("immediate", UInt64(0))] {
            // wider than any earlier test's targets, each arm in a size class of its own: no compressed memory made before
            // fits it by chance
            let width = arm == "made" ? 1024 : 1088
            let before = RawFacade.compression(of: gpu.device)
            let target = try gpu.texture(
                .bgra8Unorm, width, Self.height, usage: [.renderTarget], samples: 4, storage: .memoryless)
            if waitMs > 0 { try await Task.sleep(nanoseconds: waitMs * 1_000_000) }
            let timing = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = .dontCare
                commands.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
            }
            let after = RawFacade.compression(of: gpu.device)
            let misses = (after["misses"] ?? 0) - (before["misses"] ?? 0)
            let zbcHits = (after["zbc_hits"] ?? 0) - (before["zbc_hits"] ?? 0)
            metrics.set("\(arm)_misses", misses)
            metrics.set("\(arm)_zbc_hits", zbcHits)
            metrics.set("\(arm)_path", timing.path)
            if timing.gpuTimed { metrics.set("\(arm)_first_pass_ms", timing.gpuMilliseconds, digits: 3) }
            if arm == "made" {
                checks.append(("the first pass found its compressed memory made", misses == 0))
                checks.append(("its clear wrote comptags alone (a ZBC hit)", zbcHits > 0))
                if timing.gpuTimed {
                    checks.append(
                        ("its first pass within \(Self.clearBudgetMs) ms", timing.gpuMilliseconds <= Self.clearBudgetMs)
                    )
                }
            }
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
