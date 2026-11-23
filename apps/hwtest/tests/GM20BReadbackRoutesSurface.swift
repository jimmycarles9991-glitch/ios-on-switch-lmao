import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// gpu.gm20b.readback_routes's surface arms (gpu/SurfaceCpuRoutes.swift) over a 260x260 'BGRA' IOSurface with rows of
/// 1040 bytes the pattern is drawn into, its texture of renderTarget|shaderRead|pixelFormatView usage. The CPU routes
/// read and write its texels as rows, as an M4 Max's do (`surface_cpu_<route>_wrong` 0, `_gob` 0): getBytes of the
/// texture and of a BGRA8Unorm_sRGB view with no lock, the texture after the buffer asks, both with a read-only
/// IOSurface lock held, and the texture's read after the view's replaceRegion of the pattern's rows inverted under a
/// lock. Where the kernel laid the surface out block-linear (surface_cpu_layout 1), its pages hold GOBs, so no buffer
/// aliases them as an M4 Max's does: newBufferWithBytesNoCopy over the surface's pages and over one page inside them is
/// nil (surface_nocopy_buffer, surface_page_nocopy_buffer 0), over the process's own pages it is made
/// (own_pages_nocopy_buffer 1), and newBufferWithIOSurface: (SPI) asked twice makes none (surface_buffers 0). On
/// another layout those are reported, not judged. Their lines, a line a no-copy nil and one gm20b:buf.surface.tiled line
/// for the surface, are judged by tools/devicerun per run of this test from the pulled log (raw.nocopy-tiled-hwtest 2,
/// refusal.buf-surface-tiled-hwtest 1); this process's own log, read right after the calls, may not hold them yet, so
/// surface_nocopy_lines and surface_tiled_lines are reported only. A transfer the copy engine failed
/// (gm20b:texture.cpu.unreached) is gpu.gm20b.texture_transfer_failed's.
extension GM20BReadbackRoutesTest {
    func surfaceCpu(
        _ gpu: GPU, _ draw: (MTLTexture) async throws -> Void, _ metrics: inout Metrics,
        _ checks: inout [(String, Bool)]
    ) async throws {
        let start = Date()
        let (surface, texture) = try SurfaceCpuRoutes.surfaceTexture(gpu.device)
        try await draw(texture)
        var layout = "-"
        if let connection = GPUConnection() {
            let (kr, answer) = connection.surfaceLayout(surface)
            layout = kr == KERN_SUCCESS ? String(answer) : GPUConnection.hex(kr)
        }
        metrics.set("surface_cpu_layout", layout)
        let pattern = Self.pattern()
        let inverted = (0 ..< Self.side).reversed().flatMap { pattern[($0 * Self.row) ..< (($0 + 1) * Self.row)] }
        let r = try SurfaceCpuRoutes.run(gpu.device, surface, texture, replaced: inverted)
        let routes = ["getbytes", "view_getbytes", "after_nocopy", "locked_getbytes", "view_locked_getbytes"]
        for (route, want) in routes.map({ ($0, pattern) }) + [("view_locked_replace", inverted)] {
            let got = r.rows[route] ?? []
            let wrong = Self.off(got, want), gob = Self.gobFirstRow(want) == Array(got.prefix(Self.row))
            metrics.set("surface_cpu_\(route)_wrong", wrong)
            metrics.set("surface_cpu_\(route)_gob", gob ? 1 : 0)
            checks.append(("surface_cpu_\(route): rows (\(wrong) bytes differ, gob \(gob))", wrong == 0 && !gob))
        }
        let id = IOSurfaceGetID(unsafeBitCast(surface, to: IOSurfaceRef.self))
        let refusedNoCopy = "imported block-linear, whose pages hold its texels in GOBs: no buffer"
        let noCopyLines = ProcessLog.lines(ProcessLog.metal, "gm20b", containing: refusedNoCopy, since: start)?.count
        let tiledLines = ProcessLog.lines(
            ProcessLog.refusal,
            "gm20b",
            containing: " buf.surface.tiled: ",
            since: start
        )?
            .filter { $0.contains("of IOSurface \(id),") }.count
        metrics.set("surface_nocopy_buffer", r.surfaceNoCopy ? 1 : 0)
        metrics.set("surface_page_nocopy_buffer", r.pageNoCopy ? 1 : 0)
        metrics.set("own_pages_nocopy_buffer", r.ownNoCopy ? 1 : 0)
        metrics.set("surface_nocopy_lines", noCopyLines ?? -1)
        metrics.set("surface_buffers", r.surfaceBuffers ?? -1)
        metrics.set("surface_tiled_lines", tiledLines ?? -1)
        checks.append(("own_pages_nocopy_buffer: no buffer over the process's own pages", r.ownNoCopy))
        guard layout == "1" else { return }
        checks += [
            ("surface_nocopy_buffer: a buffer over a block-linear surface's pages", !r.surfaceNoCopy),
            ("surface_page_nocopy_buffer: a buffer over a page of them", !r.pageNoCopy),
            ("surface_buffers=\(r.surfaceBuffers ?? -1) not 0 of 2 (newBufferWithIOSurface:)", r.surfaceBuffers == 0),
        ]
    }

    static func off(_ got: [UInt8], _ want: [UInt8]) -> Int {
        guard got.count == want.count else { return want.count }
        return (0 ..< want.count).filter { $0 % 4 < 3 && got[$0] != want[$0] }.count
    }

    /// The first row's bytes of `rows` laid out block-linear at NIL's block height
    static func gobFirstRow(_ rows: [UInt8]) -> [UInt8] {
        let yLog2 = SurfaceCrossProcessTest.blockHeightLog2(side)
        let rowBytes = (row + 63) & ~63
        var tiled = [UInt8](repeating: 0, count: row)
        for y in 0 ..< side {
            for xB in 0 ..< row {
                let at = SurfaceCrossProcessTest.gobOffset(rowBytes, yLog2, xB, y)
                if at < row { tiled[at] = rows[y * row + xB] }
            }
        }
        return tiled
    }
}
