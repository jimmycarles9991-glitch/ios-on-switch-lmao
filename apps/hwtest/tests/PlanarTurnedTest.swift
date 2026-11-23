import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Turned planar render targets (gpu/PlanarTurned.swift): 550 rotations 1 and 3 over 'b3a8' planes, 550 rotation 2 and
/// 551 rotation 4 over a one-plane 'w30r', 500 rotation 1 over '420v', each a 32x64 frame over 64x64 planes, one pass
/// of a Clear and a 16x16 draw at frame (0, 0). Against an M4 Max under macOS 27.2.1 running this case code (three
/// runs, every count 0): each texture is made (planar_turned_made), every frame texel's plane bytes are the same pass's
/// unturned bytes at plane texel (63 - y, x) for rotations 1 and 4, (y, 31 - x) for 3, (31 - x, 63 - y) for 2
/// (planar_turned_off_<case>, a 4:2:0 block's chroma with its texels), and no byte past the turned frame changes
/// (planar_turned_outside).
struct PlanarTurnedTest: HWTest {
    let id = "gpu.gm20b.planar_turned"
    let name = "Turned XR and YCbCr render targets store each frame texel where Metal turns it, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            var made = true, outside = 0
            for c in PlanarTurned.cases {
                let r = try await PlanarTurned.observe(gpu, c)
                made = made && r.made
                outside += r.outside
                metrics.set("planar_turned_off_" + c.key, r.made ? r.texelsOff : -1)
                checks.append((
                    "\(c.key): \(r.made ? "\(r.texelsOff) texels off, first \(r.first)" : "no texture")",
                    r.made && r.texelsOff == 0
                ))
                if r.referenceUnstored != 0 {
                    checks.append(("\(c.key): the unturned pass left \(r.referenceUnstored) texels unstored", false))
                }
            }
            metrics.set("planar_turned_made", made)
            metrics.set("planar_turned_outside", outside)
            checks.append(("planar_turned_outside=\(outside)", outside == 0))
            return checks
        }
    }
}
