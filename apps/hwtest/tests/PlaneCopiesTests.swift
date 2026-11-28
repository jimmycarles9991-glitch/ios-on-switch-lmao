import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import HWTestKit
import Metal

/// Copies between two textures of one format over two-plane surfaces of one page (gpu/PlaneCopies.swift sameFormat):
/// 550 a (5,3) 8x4 into b, b whole into a, 551 a (2,1) 12x6 into b, 550 a whole into e (whose plane 1 ends the page),
/// e (3,0) 10x8 into a, 500 y (4,2) 8x4 into z, z whole into y. Each completes and the destination's whole allocation
/// is what an M4 Max's copies leave there under macOS 27.2.1 (three runs, every case 0 bytes off): plane 1 moved a
/// byte a texel at plane 0's rows & ~15 for the region's rows, never halved, on through plane 1's end, 0 read past the
/// source's page, nothing written past the destination's. planes_plane1_bytes_off_<case>, planes_plane1_tail_off (of
/// those, bytes past the destination's plane-1 rows), planes_plane1_status (completed copies of 7).
struct PlanesPlaneOneRowsTest: HWTest {
    let id = "gpu.gm20b.planes_plane1_rows"
    let name = "Copies between textures held in two planes move plane 1 at plane 0's rows as Metal does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            try await PlaneCopiesRun.judged(gpu, PlaneCopies.sameFormat, prefix: "planes_plane1", &metrics)
        }
    }
}

/// Copies between textures of two pixel formats, one held in two planes (gpu/PlaneCopies.swift acrossFormats), each
/// Metal's validation error: an R8Unorm over a buffer (2,1) 4x4 into 550 a (3,2), 550 a into 551 b, 551 b into 550 a
/// and 500 y into 550 b, each (2,2) 4x4 to (4,2). An M4 Max with validation off completes each and leaves the
/// destination's whole allocation as its rule says (macOS 27.2.1, three runs, 0 bytes off): plane 0 in the
/// destination's unit at each side's plane-0 rows, the buffer read in that unit at its own rows; plane 1 between two
/// held in planes as gpu.gm20b.planes_plane1_rows has it, untouched from a buffer. The driver counts each copy skipped
/// (planes_across_skipped 4, skipped.call.blit) and logs gm20b:blit.invalid "between textures of two pixel formats, one
/// held through planes: its bytes copied" once a process. planes_across_bytes_off_<case>, planes_across_status.
struct PlanesAcrossFormatsTest: HWTest {
    let id = "gpu.gm20b.planes_across_formats"
    let name = "Copies across pixel formats with a texture in planes move Metal's bytes and count as skipped, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    static let line = "between textures of two pixel formats, one held through planes: its bytes copied"

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let before = GM20BFacade.counters(of: gpu.device)
            var checks = try await PlaneCopiesRun.judged(
                gpu, PlaneCopies.acrossFormats, prefix: "planes_across", &metrics
            )
            let counts = FailedCounts(before: before, after: GM20BFacade.counters(of: gpu.device))
            metrics.set("planes_across_skipped", counts.skippedPredicates["call.blit"] ?? 0)
            checks.append(counts.skipCheck(expecting: ["call.blit": PlaneCopies.acrossFormats.count]))
            let lines = PlanesBlitsTest.refusalLines("blit.invalid")?.filter { $0.contains(Self.line) }
            metrics.set("planes_across_lines", lines?.count ?? -1)
            let logged = "gm20b:blit.invalid across formats logged once (\(lines?.count ?? -1) lines)"
            checks.append((logged, lines?.count == 1))
            return checks
        }
    }
}

enum PlaneCopiesRun {
    /// Each case's bytes off the rule as `<prefix>_bytes_off_<case>`, the count of completed copies as
    /// `<prefix>_status`, and the checks
    static func judged(
        _ gpu: GPU, _ cases: [PlaneCopies.Case], prefix: String, _ metrics: inout Metrics
    ) async throws -> [(String, Bool)] {
        var checks: [(String, Bool)] = []
        var completed = 0, pastPlaneOne = 0
        for c in cases {
            let r = try await PlaneCopies.run(gpu, c)
            guard r.made else {
                metrics.set("\(prefix)_bytes_off_\(c.key)", -1)
                checks.append(("\(c.key): no texture", false))
                continue
            }
            if r.path == "gm20b" { completed += 1 }
            pastPlaneOne += r.offPastPlaneOne
            metrics.set("\(prefix)_bytes_off_\(c.key)", r.off)
            checks.append(("\(c.key): \(r.path)", r.path == "gm20b"))
            checks.append(("\(c.key): \(r.off) bytes off of \(r.moved) moved", r.off == 0))
            checks.append(("\(c.key): \(r.sourceChanged) source bytes changed", r.sourceChanged == 0))
        }
        metrics.set("\(prefix)_status", completed)
        if prefix == "planes_plane1" { metrics.set("planes_plane1_tail_off", pastPlaneOne) }
        return checks
    }
}
