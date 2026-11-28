import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import HWTestKit
import Metal

/// A 550 over a 'w30r' a 554 render target was cleared in first (gpu/XrTiledSurface.swift), against an M4 Max: made
/// (xr_tiled_made); a kernel reads every texel (0.2510, 0.5, 0.7490, 0) (xr_tiled_read_off 0); after a kernel's (0.5,
/// 0.25, -0.25, 0.75) into the 550 the 554 reads (0.5, 0.2510, -0.2510, 1) everywhere (xr_tiled_store_off 0); getBytes
/// at (5, 3) is 0x27f80100, a replaceRegion of 0x2abcdef1 there reads back and the 554 reads its colour
/// (xr_tiled_cpu_off 0); a blit of 16x8 from (8, 0) into a 550 over a second such surface reads, through that one's
/// 554, what the first reads at (8, 0) (xr_tiled_blit_off 0). Within 1e-6.
struct XrTiledSurfaceTest: HWTest {
    let id = "gpu.gm20b.xr_tiled_surface"
    let name = "An XR texture over a surface the kernel laid out block-linear reads, stores, copies and blits, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let r = try await XrTiledSurface.observe(gpu)
            metrics.set("xr_tiled_made", r.made)
            guard r.made else { return [("no 550 over the tiled surface", false)] }
            let counts = [
                ("xr_tiled_read_off", r.readOff), ("xr_tiled_store_off", r.storeOff), ("xr_tiled_cpu_off", r.cpuOff),
                ("xr_tiled_blit_off", r.blitOff),
            ]
            for (key, value) in counts { metrics.set(key, value) }
            metrics.set("xr_tiled_first", r.first)
            return counts.map { ("\($0.0)=\($0.1)", $0.1 == 0) }
        }
    }
}

/// YCbCr textures over planes of rows under 16 bytes (gpu/YcbcrRowsUnder16.swift): a 500 8x8 over a '420v' of rows 8
/// and a 501 4x4 over a '2vuy' of rows 8 are made (ycbcr_rows16_made) and every texel reads as the reference of rows 16
/// whose first row is the scene's and the rest 0 (ycbcr_rows16_texels_off 0 of 64 and of 16, both formats, in three
/// runs on an M4 Max).
struct YcbcrRowsUnder16Test: HWTest {
    let id = "gpu.gm20b.ycbcr_rows_under16"
    let name = "YCbCr textures over planes of rows under 16 bytes read their first row and zeros, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            var made = true
            for packed in [false, true] {
                let key = packed ? "501" : "500"
                let off = try await YcbcrRowsUnder16.off(gpu, packed: packed)
                made = made && off != nil
                metrics.set("ycbcr_rows16_texels_off_" + key, off ?? -1)
                checks.append(("\(key): \(off.map { "\($0) texels off" } ?? "no texture")", off == 0))
            }
            metrics.set("ycbcr_rows16_made", made)
            return checks
        }
    }
}
