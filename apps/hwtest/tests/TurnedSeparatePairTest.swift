import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// A Depth32Float or Depth16Unorm texture and a Stencil8 texture as the depth and stencil attachments of passes into a
/// turned drawable (rotation SPI, rotations 1 to 4): the first pass writes stencil 0xA5 at depth 0.25 in one rectangle and
/// 0x5A at 0.5 behind it and stores both, the second loads them and draws red where the stencil is 0xA5, green where 0x5A,
/// then blue alone past depth 0.375. Each texture is moved into a stand-in of the plane by turn copies (turned_pass.h);
/// before, the pass went to m2v (pass.ds: a turned separate pair). wrong_r<turn>_<cell> counts frame pixels off Metal's
/// store of the same passes into a plain texture (0 to pass), path_r<turn>_<cell> the turned run's path (raw to pass).
struct TurnedSeparatePairTest: HWTest {
    let id = "gpu.gm20b.turned_separate_pair"
    let name = "A separate depth and stencil pair beside a turned drawable, every rotation, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        let states = try RotatedStates(gpu)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for turn in 1 ... 4 {
            let cells = RotatedCells(gpu: gpu, states: states, turn: turn, width: 96, height: 64, aligned: true)
            guard let plane = try cells.makePlane() else {
                return .fail("the device makes no texture with the rotation SPI", metrics, proof: gpu.proof)
            }
            for cell in RotatedCell.pairCells {
                let key = "r\(turn)_\(cell.rawValue.lowercased())"
                let result = try await cells.run(cell, into: plane)
                metrics.set("wrong_" + key, result.wrong)
                metrics.set("path_" + key, result.path)
                checks.append(("\(key): Metal's store, on the driver", result.wrong == 0 && result.path == "gm20b"))
            }
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
