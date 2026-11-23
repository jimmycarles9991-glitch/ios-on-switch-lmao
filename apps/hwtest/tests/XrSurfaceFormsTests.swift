import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// 550 over each plane of a three-plane 'b3a8' 32x8 (gpu/XrPlaneReads.swift nextPlane), against an M4 Max: over planes
/// 0 and 1 a kernel reads each word's colour with the next plane's byte as alpha, over plane 2 alpha 0
/// (xr_next_plane_texels_off, xr_next_plane_last_alpha_off 0, every channel within 1e-6); a kernel's (0.5, 0.25,
/// -0.25, 0.75) into the 550 over plane 1 writes plane 1's words 0x27f80100, plane 2's first 32 bytes a row 0xbf and
/// nothing else (xr_next_plane_store_bytes_off 0).
struct XrNextPlaneTest: HWTest {
    let id = "gpu.gm20b.xr_next_plane"
    let name = "An XR texture over plane p of three takes plane p + 1 as its alpha, reading and storing, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let r = try await XrPlaneReads.nextPlane(gpu)
            let counts = [
                ("xr_next_plane_texels_off", r.texelsOff), ("xr_next_plane_last_alpha_off", r.lastAlphaOff),
                ("xr_next_plane_store_bytes_off", r.storeBytesOff),
            ]
            for (key, value) in counts { metrics.set(key, value) }
            return counts.map { ("\($0.0)=\($0.1)", $0.1 == 0) }
        }
    }
}

/// YCBCR8_420_2P textures over other surfaces (gpu/YcbcrAnySurface.swift), each texel against the reference '420v' an
/// M4 Max reads alike (0 of 512 off for each): x420 and xf20 (rows 64; xf20 against a '420f'), bgra (a one-plane
/// 'BGRA' of rows 128: its rows as luma, chroma zeros), y3p8 (plane 1 of three: luma plane 1, chroma plane 2), rows40
/// (a '420v' of rows 40 reads rows of 32). A Clear (0.25, 0.5, 0.75, 1) into a 500 over the rows-40 '420v' changes only
/// bytes [0, 512) and [640, 896), and over the 'BGRA' the first 32 bytes of each row alone, each 115
/// (ycbcr_store_rows_bytes_outside 0).
struct YcbcrAnySurfaceTest: HWTest {
    let id = "gpu.gm20b.ycbcr_any_surface"
    let name = "YCbCr textures over 10-bit, BGRA, three-plane and rows-40 surfaces read and store as Metal does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 45 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            for scene in YcbcrAnySurface.scenes {
                let off = try await YcbcrAnySurface.off(gpu, scene)
                metrics.set("ycbcr_any_surface_off_" + scene, off ?? -1)
                checks.append(("\(scene): \(off.map { "\($0) texels off" } ?? "no texture")", off == 0))
            }
            let outside = try await YcbcrAnySurface.storeOutside(gpu)
            metrics.set("ycbcr_store_rows_bytes_outside", outside)
            checks.append(("ycbcr_store_rows_bytes_outside=\(outside)", outside == 0))
            return checks
        }
    }
}

/// 550 32x8 over plane 0 of a 'b3a8' of rows 136 (plane 1 rows 40) and over a one-plane 'w30r' of rows 136
/// (gpu/XrPlaneReads.swift off16), against an M4 Max: a kernel read and a blit to a buffer take the words at rows of 128
/// (rows rounded down to 16 bytes) and the alpha bytes at rows of 32 (xr_off16_gpu_off, xr_off16_blit_off 0), while
/// getBytes reads and replaceRegion writes the rows of 136 (xr_off16_cpu_off 0); each count is both surfaces'.
struct XrPlanesOff16Test: HWTest {
    let id = "gpu.gm20b.xr_planes_off16"
    let name = "XR planes whose rows are off 16 bytes: the GPU's rows rounded down, the CPU's as stated, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var total = (gpu: 0, blit: 0, cpu: 0)
            for planar in [true, false] {
                let r = try await XrPlaneReads.off16(gpu, planar: planar)
                total = (total.gpu + r.gpuOff, total.blit + r.blitOff, total.cpu + r.cpuOff)
            }
            let counts = [
                ("xr_off16_gpu_off", total.gpu),
                ("xr_off16_blit_off", total.blit),
                ("xr_off16_cpu_off", total.cpu),
            ]
            for (key, value) in counts { metrics.set(key, value) }
            return counts.map { ("\($0.0)=\($0.1)", $0.1 == 0) }
        }
    }
}

/// A 550 64x64 written by a kernel over a 'w30r' (gpu/XrPlaneReads.swift alphaSink), against an M4 Max: the command
/// buffer completes (it failed Internal before), every word is 0x27f80100 (xr_alpha_sink_words_off 0), nothing past the
/// rows changes, and read back the texels are (0.5, 0.2510, -0.2510, 0) (xr_alpha_sink_read_alpha 0).
struct XrAlphaSinkTest: HWTest {
    let id = "gpu.gm20b.xr_alpha_sink"
    let name = "A kernel store into an XR texture over a plane with no alpha plane keeps the colour, alpha 0, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let r = try await XrPlaneReads.alphaSink(gpu)
            metrics.set("xr_alpha_sink_status", r.status)
            guard r.status == "completed" else { return [("the store failed: \(r.status)", false)] }
            metrics.set("xr_alpha_sink_words_off", r.wordsOff)
            metrics.set("xr_alpha_sink_past_changed", r.pastChanged)
            metrics.set("xr_alpha_sink_read_off", r.readOff)
            metrics.set("xr_alpha_sink_read_alpha", Double(r.alpha), digits: 4)
            return [
                ("xr_alpha_sink_words_off=\(r.wordsOff)", r.wordsOff == 0),
                ("xr_alpha_sink_past_changed=\(r.pastChanged)", r.pastChanged == 0),
                ("xr_alpha_sink_read_off=\(r.readOff) (alpha \(r.alpha))", r.readOff == 0),
            ]
        }
    }
}
