import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import HWTestKit
import Metal

/// One-plane YCbCr render targets (gpu/YcbcrOnePlaneTargets.swift), each against the whole allocation as an M4 Max
/// leaves it, every byte (bytes_off 0 for all five on the Mac): pairs_clear, 501 64x16 over a '2vuy' of pitch 128:
/// every pair 64 af 64 93; pairs_rev_clear, 562 over a 'yuvs': af 64 93 64; pairs_odd, 501 33x16 made ShaderRead
/// alone: pair 16 (bytes 64..67) 64 af <kept> 93, bytes 68..127 of every row kept; pairs_odd_drawn, the same drawn:
/// Metal's BT.601 video-range codes laid Y0 Cb Y1 Cr (row 0 bytes 60..67: 33 a8 34 79 35 aa <kept> 79), pair 16's
/// chroma from texel 32 alone; words_clear, 504 33x16 over a 'v410' of pitch 256: words 0..32 of every row 8e e9 ca 24,
/// the rest kept. <case>_status: 0 completed, 1 the command buffer failed, 2 no texture; <case>_bytes_off: bytes not
/// Apple's.
struct YcbcrOnePlaneTargetsTest: HWTest {
    let id = "gpu.gm20b.ycbcr_onep_targets"
    let name = "One-plane YCbCr targets (2vuy, yuvs, v410) cleared and drawn, odd widths, every byte Apple's, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 45 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for c in YcbcrOnePlaneTargets.cases {
            do {
                let r = try await YcbcrOnePlaneTargets.observe(gpu, c)
                metrics.set(c.key + "_status", r.status)
                metrics.set(c.key + "_bytes_off", r.bytesOff)
                let label = r.status == 1 ? "\(c.key): failed \(r.path)" : "\(c.key): status \(r.status), first off \(r.first)"
                checks.append((label, r.status == 0 && r.bytesOff == 0))
            } catch {
                checks.append(("\(c.key): \(GPU.describe(error))", false))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
