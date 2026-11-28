import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import HWTestKit
import Metal

/// The store order of a turned YCbCr target whose rows overlap (gpu/YcbcrTurnedOverlapOrder.swift): a 540 over a '420v'
/// of luma rows 136 bytes long and 16 apart, at rotations 0 to 3, one draw of per-texel codes from the frame. Against
/// an M4 Max under macOS 27.2.1 running this case code (five runs: 1744 judged luma bytes a rotation, 0 misses), every
/// judged byte holds the code of a frame tile's winner: of the rows reaching it from one 32x32 tile of the frame, the
/// frame row in the later row pair, a pair's even row after its odd one (ycbcr_turned_order_misses_r<rotation>,
/// ycbcr_turned_order_judged_r<rotation>; -1 misses: no texture). Rotation 0 is the unturned control. The overlapping
/// rows force a luma plane declared 16 wide (IOSurfaceCreate makes no plane wider than its bytesPerRow), and an M4 Max
/// makes the turned texture over it at any extent.
struct YcbcrTurnedOverlapOrderTest: HWTest {
    let id = "gpu.gm20b.ycbcr_turned_overlap_order"
    let name = "A turned YCbCr target whose rows overlap keeps each byte from its frame tile's last store, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            for turn in YcbcrTurnedOverlapOrder.turns {
                let r = try await YcbcrTurnedOverlapOrder.observe(gpu, turn: turn)
                metrics.set("ycbcr_turned_order_judged_r\(turn)", r.judged)
                metrics.set("ycbcr_turned_order_misses_r\(turn)", r.made ? r.misses : -1)
                if r.path.hasPrefix("error:") {
                    checks.append(("rotation \(turn): the pass failed (\(r.path))", false))
                    continue
                }
                let off = "\(r.misses) of \(r.judged) bytes off the frame tiles' winners, first \(r.first)"
                let seen = r.made ? off : "no texture"
                checks.append(("rotation \(turn): \(seen)", r.made && r.judged > 0 && r.misses == 0))
            }
            return checks
        }
    }
}
