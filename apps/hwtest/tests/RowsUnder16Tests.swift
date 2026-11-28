import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import HWTestKit
import Metal

/// RGB10A8_2P_XR10 (550) 2x8 over a 'b3a8' of rows 8 and 2 in 8 MiB + 16 KiB (gpu/XrRowsUnder16.swift), each value an
/// M4 Max's under macOS 27.2.1 (this scene on it, three runs): made (xr_rows16_made); a kernel reads texel (x, y)'s
/// word at y MiB + 4x and alpha at 64 + y MiB + x (xr_rows16_read_off 0 of 16 texels, within half a code); a clear of
/// (0.5, 0.25, -0.25, 0.75) over 0xff changes exactly the 72 bytes of rows stored 16 apart, 0x27f80100 twice at [16y,
/// 16y + 8) and 0xbf at [128 + 16k, 130 + 16k) (xr_rows16_clear_off 0); read again, every texel is the read rule's over
/// those bytes (xr_rows16_read_after_off 0), texel (0, 0) alone the clear (xr_rows16_read_after 1); a whole-target
/// coded draw stores words at 16y + 4x over alpha bytes at 64 + 16y + x, the later word row winning where they meet
/// (xr_rows16_draw_off 0); a coded kernel store does the same, its word rows landing last there too (one simdgroup, the
/// later quadgroup's store wins: xr_rows16_kernel_off 0, six runs).
struct XrRowsUnder16Test: HWTest {
    let id = "gpu.gm20b.xr_rows_under_16"
    let name = "RGB10A8_2P_XR10 over planes of rows under 16 bytes reads a MiB a row apart and stores 16 apart, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let r = try await XrRowsUnder16.observe(gpu)
            metrics.set("xr_rows16_made", r.made)
            guard r.made else { return [("no 550 over planes of rows 8 and 2", false)] }
            let counts = [
                ("xr_rows16_read_off", r.readOff), ("xr_rows16_clear_off", r.clearOff),
                ("xr_rows16_read_after_off", r.readAfterOff), ("xr_rows16_draw_off", r.drawOff),
                ("xr_rows16_kernel_off", r.kernelOff),
            ]
            for (key, value) in counts { metrics.set(key, value) }
            metrics.set("xr_rows16_read_after", r.readAfterClear)
            return counts.map { ("\($0.0)=\($0.1)", $0.1 == 0) }
                + [("xr_rows16_read_after=\(r.readAfterClear)", r.readAfterClear == 1)]
                + r.failures.map { ($0, false) }
        }
    }
}

/// YCBCR8_420_2P (500) 8x8 targets over '420v' planes of rows under 16 bytes in 8 MiB + 16 KiB
/// (gpu/YcbcrRowsUnder16Stores.swift), luma and chroma rows 8 and 8, 8 and 16, 16 and 8, each value an M4 Max's under
/// macOS 27.2.1 (this scene on it, three runs): a clear pass and a whole-target draw leave the first 16 KiB as the same
/// pass leaves a 500 of rows 16 and 16, every row stored 16 bytes apart, and nothing past it changes
/// (ycbcr_rows16_clear_off_<shape>, ycbcr_rows16_draw_off_<shape> 0); a loaded pass whose draw discards every fragment
/// stores the rows read a MiB apart at the rows 16 apart, as such a pass stores a rows-16 target holding those rows
/// (ycbcr_rows16_load_moved 0), and without its draw stores nothing (ycbcr_rows16_load_no_draw 0); a blit from a buffer
/// lands its rows 16 apart, one into a buffer reads them a MiB apart, getBytes and replaceRegion use the stated rows of
/// 8 (ycbcr_rows16_blit_off_<call> 0).
struct YcbcrRowsUnder16StoresTest: HWTest {
    let id = "gpu.gm20b.ycbcr_rows_under_16_stores"
    let name = "YCbCr targets of rows under 16 bytes store their rows 16 apart as Metal does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let r = try await YcbcrRowsUnder16Stores.observe(gpu)
            guard r.made else { return [("no 500 over planes of rows under 16 bytes", false)] }
            var counts: [(String, Int)] = []
            for (key, _, _) in YcbcrRowsUnder16Stores.shapes {
                counts.append(("ycbcr_rows16_clear_off_" + key, r.clearOff[key] ?? -1))
                counts.append(("ycbcr_rows16_draw_off_" + key, r.drawOff[key] ?? -1))
            }
            counts.append(("ycbcr_rows16_load_moved", r.loadMovedOff))
            counts.append(("ycbcr_rows16_load_no_draw", r.loadNoDrawOff))
            for (call, off) in r.blitOff.sorted(by: { $0.key < $1.key }) {
                counts.append(("ycbcr_rows16_blit_off_" + call, off))
            }
            for (key, value) in counts { metrics.set(key, value) }
            return counts.map { ("\($0.0)=\($0.1)", $0.1 == 0) } + r.failures.map { ($0, false) }
        }
    }
}
