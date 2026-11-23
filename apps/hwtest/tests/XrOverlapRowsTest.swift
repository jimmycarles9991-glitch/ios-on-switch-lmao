import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// RGB10A8_2P_XR10 over 'b3a8' planes whose rows overlap inside a plane (gpu/XrOverlapRows.swift), against an M4 Max
/// under macOS 27.2.1 running this case code (four runs, every count 0): a kernel reads each texel at the planes' rows
/// over rows 64 and 16, rows 64 and 32, and planes of 32x4 for a 32x8 texture (xr_overlap_read_off_<scene>); written
/// over rows 64 and 16 (32x8) and rows 128 and 64 (64x64), a pass's per-row words and alpha bytes keep the row whose
/// store lands last by overlap_order.h overlapOrder (xr_overlap_pass_rows_off), a kernel's keep a row that reaches
/// them (xr_overlap_kernel_rows_uncovered), a Clear leaves 0x27f80100 and 0xbf (xr_overlap_clear_off), and no byte
/// outside the planes' rows changes (xr_overlap_outside). Each texture is made (nil reads -1).
struct XrOverlapRowsTest: HWTest {
    let id = "gpu.gm20b.xr_overlap_rows"
    let name = "An XR texture over planes whose rows overlap reads, draws, stores and clears as Metal does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            for scene in XrOverlapRows.reads {
                let off = try await XrOverlapRows.readOff(gpu, scene)
                metrics.set("xr_overlap_read_off_" + scene.key, off ?? -1)
                checks.append(("read \(scene.key): \(off.map { "\($0) texels off" } ?? "no texture")", off == 0))
            }
            var passOff = 0, kernelUncovered = 0, clearOff = 0, outside = 0
            var first = "-"
            for scene in XrOverlapRows.written {
                guard let r = try await XrOverlapRows.observe(gpu, scene) else {
                    checks.append(("written \(scene.key): no texture", false))
                    continue
                }
                passOff += r.passOff
                kernelUncovered += r.kernelUncovered
                clearOff += r.clearOff
                outside += r.outside
                if first == "-" && r.first != "-" { first = scene.key + ":" + r.first }
            }
            let counts = [
                ("xr_overlap_pass_rows_off", passOff), ("xr_overlap_kernel_rows_uncovered", kernelUncovered),
                ("xr_overlap_clear_off", clearOff), ("xr_overlap_outside", outside),
            ]
            for (key, value) in counts {
                metrics.set(key, value)
                checks.append(("\(key)=\(value)", value == 0))
            }
            metrics.set("xr_overlap_first", first)
            return checks
        }
    }
}
