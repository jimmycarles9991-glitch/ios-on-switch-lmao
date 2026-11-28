import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import HWTestKit
import Metal

/// An 80-texel BGRA8Unorm texel buffer over a 'BGRA' 64x16 of rows 320 a BGRA8 target drew into first, which on the
/// device the kernel lays out block-linear (gpu/TexelRows.swift; Metal makes such a buffer up to one row, 80 texels,
/// and asserts past it). Each step as an M4 Max under macOS 27.2.1 does it (this scene on it, three runs, every step 0
/// off; its surfaces are linear, so this is the in-order rule the driver's pool keeps): the buffer reads row 0's
/// texels in order (texel_reads_ok) and 0 in the row's padding (padding_reads_zero); a kernel's store, a fragment
/// function's store and a pass rendering into the buffer (elements 20-59 green) land in row 0 (kernel_stores_ok,
/// fragment_stores_ok, attachment_ok); getBytes of elements 10-59 reads row 0 (cpu_get_ok) and replaceRegion writes it
/// (cpu_replace_ok); rows 1-15 keep the draw's texels throughout (outside_range_unchanged).
struct TexelRowsTest: HWTest {
    let id = "gpu.gm20b.texel_rows"
    let name = "A texel buffer over a block-linear surface reads and writes its row in order, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let r = try await TexelRows.observe(gpu)
            metrics.set("texel_rows_made", r.made)
            guard r.made else { return [("no BGRA8 texel buffer over the surface", false)] }
            let steps = [
                ("texel_reads_ok", r.readsOff), ("padding_reads_zero", r.paddingOff), ("kernel_stores_ok", r.kernelOff),
                ("fragment_stores_ok", r.fragmentOff), ("attachment_ok", r.attachmentOff), ("cpu_get_ok", r.getOff),
                ("cpu_replace_ok", r.replaceOff), ("outside_range_unchanged", r.outsideOff),
            ]
            for (key, off) in steps { metrics.set(key, off == 0) }
            return steps.map { ("\($0.0): \($0.1) texels off", $0.1 == 0) }
                + r.paths.filter { $0 != "gm20b" }.map { ("a command buffer \($0)", false) }
        }
    }
}

/// A 128-texel RGB10A8_2P_XR10 texel buffer over a 'w30r' 64x64 a BGR10_XR target drew into first (block-linear on the
/// device), read by a kernel after another wrote (0.5, 0.25, -0.25, 0.75) through the 554 (gpu/XrStandInRefill.swift):
/// in the same compute encoder after a texture memory barrier (xr_refill_same_encoder_off) and, as the control, in an
/// encoder of its own (xr_refill_next_encoder_off). An M4 Max under macOS 27.2.1 reads the written colour with alpha 0
/// both ways (this scene on it, three runs, 0 of 128 off).
struct XrStandInRefillTest: HWTest {
    let id = "gpu.gm20b.xr_stand_in_refill"
    let name = "An XR texel buffer over a tiled surface reads a write made earlier in the same compute encoder, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            for (same, key) in [(true, "xr_refill_same_encoder_off"), (false, "xr_refill_next_encoder_off")] {
                do {
                    let r = try await XrStandInRefill.observe(gpu, sameEncoder: same)
                    metrics.set(key, r.made ? r.off : -1)
                    checks += r.made
                        ? [("\(key): \(r.path)", r.path == "gm20b"), ("\(key)=\(r.off)", r.off == 0)]
                        : [("\(key): no 550 texel buffer over the surface", false)]
                } catch {
                    metrics.set(key, -1)
                    checks.append(("\(key): \(GPU.describe(error))", false))
                }
            }
            return checks
        }
    }
}
