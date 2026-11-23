import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A 540 target over a '420v' whose rows overlap (gpu/YcbcrOverlapRows.swift): a, 64x16 of pitch 16, and b, 64x64 of
/// pitch 16, hold in every luma and chroma byte the code of the row overlap_order.h names, as an M4 Max's four runs
/// each did (<shape>_luma_off, <shape>_chroma_off 0); c, 128x17 of pitch 32, two tiles across that share bytes, which an
/// M4 Max stores in no fixed order (64 to 320 luma and 64 chroma bytes off that order in four runs): reported. Every
/// shape: the pass completes (status 0; it failed Internal before the driver drew overlapping rows), and no byte past
/// the last row's end changes (past_changed 0, as on the M4 Max).
struct YcbcrOverlapRowsTest: HWTest {
    let id = "gpu.gm20b.ycbcr_overlap_rows"
    let name = "A YCbCr target whose plane rows overlap keeps the row an M4 Max stores last in each byte, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 45 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            for shape in YcbcrOverlapRows.shapes {
                let r = try await YcbcrOverlapRows.observe(gpu, shape)
                for (key, value) in [
                    ("status", r.status),
                    ("luma_off", r.lumaOff),
                    ("chroma_off", r.chromaOff),
                    ("past_changed", r.pastChanged),
                ] {
                    metrics.set("\(shape.key)_\(key)", value)
                }
                checks.append((
                    "\(shape.key): status \(r.status) \(r.path), past_changed \(r.pastChanged)",
                    r.status == 0 && r.pastChanged == 0
                ))
                if shape.key != "c" {
                    checks.append((
                        "\(shape.key): luma_off \(r.lumaOff) chroma_off \(r.chromaOff), first \(r.first)",
                        r.lumaOff == 0 && r.chromaOff == 0
                    ))
                }
            }
            return checks
        }
    }
}

/// A 504 target of 96 x h over a 'v410' whose rows overlap (gpu/YcbcrOverlapWords.swift), p256 (h 33) and p320 (h 64):
/// the pass completes (status 0) and no byte past the last row's end changes (past_changed 0), as on an M4 Max. Which row
/// each overlapped byte keeps is reported (bytes_off against overlap_order.h): an M4 Max's runs differ there (p256:
/// 0 in three runs of four, 1,894 in one; p320: 125 or 126 in all four).
struct YcbcrOverlapWordsTest: HWTest {
    let id = "gpu.gm20b.ycbcr_overlap_words"
    let name = "A 10-bit 4:4:4 YCbCr target whose rows overlap completes and writes nothing past its rows, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 45 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            for shape in YcbcrOverlapWords.shapes {
                let r = try await YcbcrOverlapWords.observe(gpu, shape)
                metrics.set("\(shape.key)_status", r.status)
                metrics.set("\(shape.key)_bytes_off", r.bytesOff)
                metrics.set("\(shape.key)_past_changed", r.pastChanged)
                checks.append((
                    "\(shape.key): status \(r.status) \(r.path), past_changed \(r.pastChanged)",
                    r.status == 0 && r.pastChanged == 0
                ))
            }
            return checks
        }
    }
}
