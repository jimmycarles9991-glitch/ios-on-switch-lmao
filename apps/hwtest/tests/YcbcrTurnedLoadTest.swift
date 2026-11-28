import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import HWTestKit
import Metal

/// Passes that load and store a YCbCr 4:2:0 (500) target turned by rotation 1, 2 or 3 over plane 0 of a '420v' 128x64
/// (gpu/YcbcrTurnedLoad.swift), one draw of a frame rectangle whose fragments are all discarded, over whole frames
/// (64x128, 128x64) and frames of no whole tiles (56x120, 120x56, a second rectangle at (40, 40)). An M4 Max under
/// macOS 27.2.1 (this test's scene code, three runs alike) completes each and stores back the 32x32 tile of the frame
/// the draw reaches, from the frame's origin, turned into the plane: no luma byte outside that box changes, no chroma
/// byte outside its rows and bytes, nothing past the planes (ycbcr_turned_load_box_off_<case> 0,
/// ycbcr_turned_load_bytes_outside 0). Each pass failed its command buffer at record before (pass.ycbcr):
/// ycbcr_turned_load_status counts those completed. ycbcr_turned_load_changed: luma bytes the stores rewrote (reported;
/// an M4 Max's 406-559 a case).
struct YcbcrTurnedLoadTest: HWTest {
    let id = "gpu.gm20b.ycbcr_turned_load"
    let name = "Turned YCbCr targets a pass loads store back the frame tiles its draws reach, turned, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            var completed = 0, outside = 0, changed = 0
            for c in YcbcrTurnedLoad.cases {
                do {
                    let r = try await YcbcrTurnedLoad.observe(gpu, c)
                    guard r.made else {
                        checks.append(("\(c.key): no texture", false))
                        continue
                    }
                    guard !r.status.hasPrefix("error:") else {
                        checks.append(("\(c.key): \(r.status)", false))
                        continue
                    }
                    completed += 1
                    let off = r.lumaOutside + r.chromaOutside + r.tailChanged
                    outside += off
                    changed += r.lumaChanged
                    metrics.set("ycbcr_turned_load_box_off_" + c.key, off)
                    checks.append(
                        (
                            "\(c.key): \(r.lumaOutside) luma, \(r.chromaOutside) chroma and \(r.tailChanged) tail "
                                + "bytes changed outside \(c.box) (changed \(r.changedBox.map { "\($0)" } ?? "none"))",
                            off == 0
                        )
                    )
                } catch {
                    checks.append(("\(c.key): \(GPU.describe(error))", false))
                }
            }
            metrics.set("ycbcr_turned_load_status", completed)
            metrics.set("ycbcr_turned_load_bytes_outside", outside)
            metrics.set("ycbcr_turned_load_changed", changed)
            let all = YcbcrTurnedLoad.cases.count
            checks.append(("\(completed) of \(all) passes completed", completed == all))
            return checks
        }
    }
}
