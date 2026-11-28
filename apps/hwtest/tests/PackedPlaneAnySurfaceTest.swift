import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import HWTestKit
import Metal

/// R10Unorm_PACKED and RG10Unorm_PACKED over planes of any surface (gpu/PackedPlaneAnySurface.swift), against an M4 Max
/// under macOS 27.2.1 running this case code (three runs, every scene 0 texels off, worst 0 codes): '420v' planes 0 and
/// 1, a planeless 'BGRA' (588 and 589), 'p420' plane 1 as 588 and plane 0 as 589, an 8x8 588 over a 32x16 plane, rows
/// 88 (read at 80), plane 1 at 1544 (read at 1536), and a 589 whose last row's codes lie past the surface's last page
/// (those read 0). Each texture is made (packed_any_made) and every texel reads its code within half a code
/// (packed_any_texels_off_<scene>); packed_any_worst_codes is the largest distance.
struct PackedPlaneAnySurfaceTest: HWTest {
    let id = "gpu.gm20b.packed_plane_any_surface"
    let name = "Packed 10-bit textures over any surface plane read the codes Metal reads, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            var made = true
            var worst: Float = 0
            for scene in PackedPlaneAnySurface.scenes {
                let r = try await PackedPlaneAnySurface.observe(gpu, scene)
                made = made && r.made
                worst = max(worst, r.worst)
                metrics.set("packed_any_texels_off_" + scene.key, r.made ? r.off : -1)
                checks.append((
                    "\(scene.key): \(r.made ? "\(r.off) texels off, first \(r.first)" : "no texture")",
                    r.made && r.off == 0
                ))
            }
            metrics.set("packed_any_made", made)
            metrics.set("packed_any_worst_codes", Double(worst), digits: 3)
            return checks
        }
    }
}
