import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Plain-format textures over planes whose rows overlap or run past the allocation, read by a kernel
/// (gpu/PlainRows.swift), against an M4 Max under macOS 27.2.1 (this scene on it, ten runs, 0 off): R8Unorm 136x104
/// over a '420v' plane of rows 16 reads texel (x, y) at plane byte y * 16 + x (plain_reads_off_overlap, of 14144);
/// BGRA8Unorm 64x1048 over the one plane of a 'BGRA' 64x48 of rows 256 in 12 KiB reads its page's bytes, those past the
/// allocation too, and 0 past the page (plain_reads_off_past_page, of 67072; the driver reads those rows through a
/// sparse window). plain_reads_made_<arm>.
struct PlainRowsReadsTest: HWTest {
    let id = "gpu.gm20b.plain_rows_reads"
    let name = "Plain textures over overlapping rows and past the last page read as Metal reads them, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let arms: [(String, (GPU) async throws -> PlainRows.Reading)] = [
                ("overlap", PlainRows.overlapReads), ("past_page", PlainRows.pastPageReads),
            ]
            var checks: [(String, Bool)] = []
            for (key, arm) in arms {
                metrics.set("plain_reads_made_" + key, false)
                do {
                    let r = try await arm(gpu)
                    PlainRowsRun.report(r, "plain_reads", key, &metrics, &checks)
                } catch {
                    checks.append(("\(key): \(GPU.describe(error))", false))
                }
            }
            return checks
        }
    }
}

/// RGBA32Uint 40x40 over a '420v' plane of rows 128, every texel drawn its code by a DontCare pass (gpu/PlainRows
/// Stores.swift): each 16-byte element keeps the code of the row whose store lands last by overlap_order.h's
/// overlapOrder, an M4 Max's order (this scene on it, ten runs: every one of the 336 elements two to five rows reach
/// held the same row each run, the rows the driver's host test names, and 0 of 352 off the order; the bytes past the
/// span unchanged). plain_order_off, plain_order_made.
struct PlainRowsStoreOrderTest: HWTest {
    let id = "gpu.gm20b.plain_rows_store_order"
    let name = "A pass into plain texels over overlapping rows keeps the row Metal stores last, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let s = PlainRows.Shape(MTLPixelFormat.rgba32Uint.rawValue, 40, 40, bpp: 16, pitch: 128)
            return try await PlainRowsRun.stores(gpu, s, "plain_order", &metrics) {
                PlainRows.passed(s, $0, scissor: nil)
            }
        }
    }
}

/// R32Uint 80x40 over a '420v' plane of rows 160 (every byte 0xee), a loaded pass scissored to (8, 4) 40x20 drawing
/// every texel's code (gpu/PlainRowsStores.swift passed): each element some row in a 32x32 frame tile the scissor
/// reaches holds, from the latest of those rows by overlapOrder, that row's code inside the scissor and its loaded 0xee
/// outside; an element no such row reaches keeps 0xee (an M4 Max under macOS 27.2.1, this scene, ten runs, 0 of 1640
/// off). plain_loaded_off, plain_loaded_made.
struct PlainRowsLoadedTest: HWTest {
    let id = "gpu.gm20b.plain_rows_loaded"
    let name = "A scissored loaded pass over overlapping plain rows stores the tiles it reaches as Metal does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let s = PlainRows.Shape(MTLPixelFormat.r32Uint.rawValue, 80, 40, bpp: 4, pitch: 160)
            let scissor = MTLScissorRect(x: 8, y: 4, width: 40, height: 20)
            return try await PlainRowsRun.stores(gpu, s, "plain_loaded", scissor: scissor, &metrics) {
                PlainRows.passed(s, $0, scissor: scissor)
            }
        }
    }
}

/// R8Unorm 136x40 over a '420v' plane of rows 64 (gpu/PlainRows.swift cpuCopies), against an M4 Max under macOS 27.2.1
/// (this scene, ten runs, 0 off): replaceRegion of the whole texture at bytesPerRow 136 leaves each plane byte the code
/// of the last row reaching it and nothing past the rows changed (plain_cpu_write_off); getBytes reads texel (x, y) at
/// plane byte y * 64 + x (plain_cpu_read_off).
struct PlainRowsCpuTest: HWTest {
    let id = "gpu.gm20b.plain_rows_cpu"
    let name = "CPU copies of a plain texture over overlapping rows move bytes at the plane's rows, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let r = try PlainRows.cpuCopies(gpu)
            metrics.set("plain_cpu_made", r.made)
            guard r.made else { return [("no R8Unorm over rows of 64", false)] }
            metrics.set("plain_cpu_write_off", r.writeOff)
            metrics.set("plain_cpu_read_off", r.readOff)
            return [
                ("plain_cpu_write_off=\(r.writeOff), first \(r.first)", r.writeOff == 0),
                ("plain_cpu_read_off=\(r.readOff)", r.readOff == 0),
            ]
        }
    }
}

/// R8Unorm 8x3 over a '420v' plane of rows 8 in 2.5 MiB (gpu/PlainRowsStores.swift rowsUnder16), against an M4 Max
/// under macOS 27.2.1 (this scene, ten runs, 0 off): a kernel reads row y at y MiB + x (plain_under16_read_off),
/// getBytes at the plane's rows, y * 8 + x (plain_under16_getbytes_off), and a pass of codes stores row y at y * 16 + x
/// and nothing else in the first 16 KiB or at the mebibyte rows (plain_under16_store_off).
struct PlainRowsUnder16Test: HWTest {
    let id = "gpu.gm20b.plain_rows_under16"
    let name = "A plain texture over rows under 16 bytes reads them a MiB apart and stores them 16 apart, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let u = try await PlainRows.rowsUnder16(gpu)
            metrics.set("plain_under16_made", u.made)
            guard u.made else { return [("no R8Unorm over rows of 8", false)] }
            let counts = [
                ("plain_under16_read_off", u.readOff), ("plain_under16_getbytes_off", u.getBytesOff),
                ("plain_under16_store_off", u.storeOff),
            ]
            for (key, value) in counts { metrics.set(key, value) }
            return [("read \(u.readPath)", u.readPath == "gm20b"), ("pass \(u.passPath)", u.passPath == "gm20b")]
                + counts.map { ("\($0.0)=\($0.1)", $0.1 == 0) }
        }
    }
}

enum PlainRowsRun {
    /// `r`'s made, path and off as <prefix>_made_<key>, <prefix>_off_<key>, and its checks
    static func report(
        _ r: PlainRows.Reading, _ prefix: String, _ key: String, _ metrics: inout Metrics,
        _ checks: inout [(String, Bool)]
    ) {
        metrics.set("\(prefix)_made_\(key)", r.made)
        metrics.set("\(prefix)_off_\(key)", r.made ? r.off : -1)
        guard r.made else {
            checks.append(("\(key): no texture", false))
            return
        }
        checks.append(("\(key): \(r.path)", r.path == "gm20b"))
        checks.append(("\(key): \(r.off) of \(r.judged) off, first \(r.first)", r.off == 0))
    }

    /// `s` stored over a fresh surface (gpu/PlainRowsStores.swift stored) against `want`: <prefix>_made, <prefix>_off
    static func stores(
        _ gpu: GPU, _ s: PlainRows.Shape, _ prefix: String, scissor: MTLScissorRect? = nil, blit: Bool = false,
        _ metrics: inout Metrics, _ want: (Int) -> [UInt8]?
    ) async throws -> [(String, Bool)] {
        let stored = try await PlainRows.stored(gpu, s, scissor: scissor, blit: blit)
        let r = PlainRows.judged(s, stored, want)
        metrics.set(prefix + "_made", r.made)
        metrics.set(prefix + "_off", r.made ? r.off : -1)
        guard r.made else { return [("no texture over rows of \(s.pitch)", false)] }
        return [
            ("\(prefix): \(r.path)", r.path == "gm20b"),
            ("\(prefix): \(r.off) of \(r.judged) off, first \(r.first)", r.off == 0),
        ]
    }
}
