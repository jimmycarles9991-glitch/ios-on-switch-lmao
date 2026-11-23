import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Turned surface textures of formats the GM20B renders through a stand-in land every frame pixel where a BGRA8 texture
/// of the same rotation lands it (TurnedStandInCases.swift). Each arm draws (x/64, y/64) over its whole frame
/// (shaders/StandInFormats.metal turned_where_fragment; the R8 arm, one channel, (y * width + x) / 255 by
/// turned_index_fragment) into a texture made by Metal's rotation SPI over a surface plane, and the same draw into BGRA8
/// over a BGRA surface of that plane's size (RotatedCells.makePlane): rot1_rg8srgb (RG8Unorm_sRGB, rotation 1, a 32x16
/// surface of 2-byte elements), rot3_bgra10xrsrgb (553, rotation 3, 8-byte elements), rot2_bgr10xr (554, rotation 2,
/// a 32x16 w30r pixel buffer), rot1_r8_plane0 (R8 over plane 0 of a 32x8 420v pixel buffer, rotation 1) and
/// rot3_rg8_plane1 (RG8 over plane 1 of a 64x32 420v pixel buffer, rotation 3). Each key counts the plane texels whose
/// frame pixel differs from BGRA8's; 0 on an M4 Max for every arm (m2v-textures4 probes/reads.mm and more.mm landings,
/// and this test's case code on it). On a failure <arm>_looks= names the turn whose store map the plane matches
/// (RotatedCells.stored, r0 unturned; rotation 4 stores as rotation 1), or the nearest and its misses. Every command
/// buffer raw, failed_cbs=0.
struct TurnedStandInTargetsTest: HWTest {
    let id = "gpu.gm20b.turned_standin_targets"
    let name = "Turned stand-in and plane targets land every pixel where BGRA8 of the same rotation does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        let states = try RotatedStates(gpu)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var looks: Set<String> = []
        for arm in TurnedStandInCases.arms {
            do {
                let result = try await TurnedStandInCases.run(gpu, states, arm)
                metrics.set(arm.key, result.off)
                if result.off != 0 {
                    metrics.set("\(arm.key)_looks", result.looks)
                    looks.insert("\(arm.key)_looks")
                }
                checks.append(("\(arm.key): every pixel where BGRA8 lands it", result.off == 0))
            } catch {
                checks.append(("\(arm.key): \(GPU.describe(error))", false))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: looks.union(TurnedStandInCases.arms.map(\.key)).union(["failed_cbs"]))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
