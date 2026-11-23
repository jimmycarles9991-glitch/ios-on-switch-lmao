import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Blits into and out of Shared 552 and 553 textures (gpu/XrCopyClamp.swift), every arm against an M4 Max's: a 64x64 one
/// made with allowGPUOptimizedContents (Metal's default) keeps every field's code with its low 6 bits 0 and the alpha code
/// clamped to 384..894 (<arm>_off 0 of 16,384 fields against that; 2,052 alphas clamped), both a buffer blitted in and
/// read with getBytes (buffer_in) and replaceRegion then blitted out (blit_out), while getBytes of the replaced texture
/// keeps every field bit for bit (source_changed 0); one made without it (plain) or of 15x15 keeps every field bit for
/// bit either way. The facade's own copy: no failed command buffer, every one raw.
struct XrCopyClampTest: HWTest {
    let id = "gpu.gm20b.xr_copy_clamp"
    let name = "Blits into and out of BGRA10_XR textures clamp and drop low bits only as Metal's optimized ones do, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 45 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            for format in [UInt(552), 553] {
                for (optimized, size) in [(true, 64), (false, 64), (true, 15)] {
                    for route in XrCopyClamp.Route.allCases {
                        let arm = XrCopyClamp.Arm(format: format, optimized: optimized, size: size, route: route)
                        let r = try await XrCopyClamp.observe(gpu, arm)
                        let clamps = optimized && size == 64, off = clamps ? r.offClamped : r.offExact
                        metrics.set(arm.key + "_off", off)
                        if route == .blitOut { metrics.set(arm.key + "_source_changed", r.sourceChanged) }
                        checks.append((
                            "\(arm.key): \(off) fields off, \(r.sourceChanged) source fields changed",
                            off == 0 && r.sourceChanged == 0
                        ))
                    }
                }
            }
            return checks
        }
    }
}
