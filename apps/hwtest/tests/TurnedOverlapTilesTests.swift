import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import HWTestKit
import Metal

/// Turned targets over planes whose rows overlap, judged inside one 32x32 frame tile (gpu/TurnedOverlapTiles.swift),
/// against an M4 Max under macOS 27.2.1 (this scene on it, five runs, 0 off, every judged byte telling the frame order
/// from the plane order): a 500 turned by 3 over a '420v' of chroma rows 16 apart keeps chroma row 0's bytes 16-31 at
/// 16400-16415 and row 46's 112-127 at 17232-17247 (turned_overlap_chroma_off); a 550 turned by 1 over word rows 96
/// apart that run into the alpha plane keeps word row 1's words 0-7 at bytes 96-127 (turned_overlap_words_off), each
/// as a control of rows that do not overlap stores them. turned_overlap_status: scenes made and completed of 2. A
/// 540's luma at rotations 0-3 is gpu.gm20b.ycbcr_turned_overlap_order's.
struct TurnedOverlapTileOrderTest: HWTest {
    let id = "gpu.gm20b.turned_overlap_tile_order"
    let name = "Turned targets over overlapping rows keep the later frame row inside a tile, as Metal does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let scenes = [
                ("turned_overlap_chroma_off", TurnedOverlapTiles.chromaRows),
                ("turned_overlap_words_off", TurnedOverlapTiles.wordRows),
            ]
            return try await TurnedOverlapRun.judged(gpu, scenes, status: "turned_overlap_status", &metrics)
        }
    }
}

/// A 550 turned by 1 over a 'b3a8' whose word rows 384 bytes long lie 192 apart and run into the alpha plane at 1536
/// (gpu/TurnedOverlapTiles.swift clearOrder), cleared to (0.5, 0.25, -0.25, 0.75) alone: inside one frame tile the
/// word lands after the alpha, so bytes 1568-1597 and 1696-1706 hold 0x27f80100's bytes, not 0xbf (an M4 Max under
/// macOS 27.2.1, this scene on it, five runs, 0 off of 41 bytes; the clear-order probe's case 61). xr_clear_order_off,
/// xr_clear_order_status.
struct XrTurnedClearOrderTest: HWTest {
    let id = "gpu.gm20b.xr_turned_clear_order"
    let name = "A turned RGB10A8_2P_XR10 clear over overlapping rows lands its words after its alpha, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            try await TurnedOverlapRun.judged(
                gpu, [("xr_clear_order_off", TurnedOverlapTiles.clearOrder)], status: "xr_clear_order_status", &metrics
            )
        }
    }
}

enum TurnedOverlapRun {
    static func judged(
        _ gpu: GPU, _ scenes: [(String, (GPU) async throws -> TurnedOverlapTiles.Reading)], status: String,
        _ metrics: inout Metrics
    ) async throws -> [(String, Bool)] {
        var checks: [(String, Bool)] = []
        var completed = 0
        for (key, scene) in scenes {
            do {
                let r = try await scene(gpu)
                metrics.set(key, r.made ? r.off : -1)
                guard r.made else {
                    let why = "no turned texture over a plane \(r.planeWidth) wide (frame \(r.frameExtent))"
                    checks.append(("\(key): \(why)", false))
                    continue
                }
                if r.path == "gm20b" { completed += 1 }
                checks += [
                    ("\(key): \(r.path)", r.path == "gm20b"), ("\(key)=\(r.off) of \(r.telling) telling", r.off == 0),
                ]
            } catch {
                metrics.set(key, -1)
                checks.append(("\(key): \(GPU.describe(error))", false))
            }
        }
        metrics.set(status, completed)
        return checks
    }
}
