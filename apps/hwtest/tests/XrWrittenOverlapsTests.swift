import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A 550 32x8, ShaderRead, ShaderWrite and RenderTarget, over plane 0 of a 'b3a8' 32x4 (words rows 128 at 0, alpha rows
/// 32 at 512, 1024 bytes; gpu/XrWrittenOverlaps.swift intoAlpha), whose word rows 4 to 7 run into the alpha plane and
/// past it. An M4 Max under macOS 27.2.1 (this test's scene code, three runs alike): made (xr_into_alpha_made; nil
/// before); every texel a kernel reads is the word at y * 128 + 4x with the alpha byte at 512 + y * 32 + x
/// (xr_into_alpha_read_off 0 of 256); a clear to (0.5, 0.25, -0.25, 0.75) leaves words 0x27f80100 everywhere but alpha
/// rows 4 to 7 (bytes 640 to 767), which hold 0xbf but row 5's first ten bytes, which hold the words' bytes
/// (xr_into_alpha_clear_bytes_off 0 of the 1023 bytes but the tie byte, gpu.gm20b.xr_into_alpha_tie's), and nothing past
/// the planes (xr_into_alpha_tail 0).
struct XrWordsIntoAlphaTest: HWTest {
    let id = "gpu.gm20b.xr_words_into_alpha"
    let name = "A written RGB10A8_2P_XR10 whose words run into its alpha plane reads and clears as an M4 Max's, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let r = try await XrWrittenOverlaps.intoAlpha(gpu)
            metrics.set("xr_into_alpha_made", r.made)
            guard r.made else { return [("no 550 over the 'b3a8' 32x4", false)] }
            metrics.set("xr_into_alpha_read_off", r.readOff)
            guard !r.status.hasPrefix("error:") else {
                return [("xr_into_alpha_read_off=\(r.readOff)", r.readOff == 0), ("the clear: \(r.status)", false)]
            }
            metrics.set("xr_into_alpha_clear_bytes_off", r.clearOff)
            metrics.set("xr_into_alpha_tail", r.tailChanged)
            return [
                ("xr_into_alpha_read_off=\(r.readOff)", r.readOff == 0),
                ("xr_into_alpha_clear_bytes_off=\(r.clearOff)", r.clearOff == 0),
                ("xr_into_alpha_tail=\(r.tailChanged)", r.tailChanged == 0),
            ]
        }
    }
}

/// gpu.gm20b.xr_words_into_alpha's clear at its tie byte (682: word row 5's byte 42 and alpha texel (10, 5), the one byte
/// a word and an alpha byte of the same texel column both store to): an M4 Max under macOS 27.2.1 leaves the OR of the
/// two, 0xff (0xf8 | 0xbf; this test's scene code, three runs alike) (xr_into_alpha_tie).
struct XrIntoAlphaTieTest: HWTest {
    let id = "gpu.gm20b.xr_into_alpha_tie"
    let name = "Where a written RGB10A8_2P_XR10's word and alpha stores meet in a byte, a clear leaves their OR, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let r = try await XrWrittenOverlaps.intoAlpha(gpu)
            guard r.made else { return [("no 550 over the 'b3a8' 32x4", false)] }
            guard !r.status.hasPrefix("error:") else { return [("the clear: \(r.status)", false)] }
            let want = Int(XrWrittenOverlaps.intoAlphaCleared()[XrWrittenOverlaps.tieByte])
            metrics.set("xr_into_alpha_tie", String(format: "%02x", r.tie))
            let got = String(format: "%02x", r.tie), apple = String(format: "%02x", want)
            return [("byte \(XrWrittenOverlaps.tieByte) 0x\(got), not 0x\(apple)", r.tie == want)]
        }
    }
}

/// A 550 32x8, ShaderRead and ShaderWrite, over a 'w30r' of one 16 KiB page whose one plane of rows 64 starts 256 bytes
/// before its end (gpu/XrWrittenOverlaps.swift alonePastLastPage): its rows overlap and rows 3 to 7 run past the page.
/// An M4 Max under macOS 27.2.1 (this test's scene code, three runs alike): made (xr_alone_window_made; nil before); a
/// kernel reads the word at the plane + y * 64 + 4x inside the page and 0 past it, alpha 0 (xr_alone_window_read_off 0
/// of 256); a kernel's store of (0.5, 0.25, -0.25, 0.75) completes (xr_alone_window_status) and leaves every word of
/// the plane inside the page 0x27f80100 and every byte before the plane as it was (xr_alone_window_store_off 0).
struct XrAlonePastLastPageTest: HWTest {
    let id = "gpu.gm20b.xr_alone_past_last_page"
    let name = "A written RGB10A8_2P_XR10 over a plane alone whose rows run past the last page reads and stores, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let r = try await XrWrittenOverlaps.alonePastLastPage(gpu)
            metrics.set("xr_alone_window_made", r.made)
            guard r.made else { return [("no 550 over the plane past the page", false)] }
            metrics.set("xr_alone_window_read_off", r.readOff)
            metrics.set("xr_alone_window_status", r.status.hasPrefix("error:") ? r.status : "completed")
            metrics.set("xr_alone_window_store_off", r.storeOff)
            return [
                ("xr_alone_window_read_off=\(r.readOff)", r.readOff == 0),
                ("the store: \(r.status)", !r.status.hasPrefix("error:")),
                ("xr_alone_window_store_off=\(r.storeOff)", r.storeOff == 0),
            ]
        }
    }
}
