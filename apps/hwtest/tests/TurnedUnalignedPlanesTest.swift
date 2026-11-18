import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.x1.rotated_target's cells with a second turned plane loaded beside the target (besideR4, turns, frames), over its
/// 90x60 frame, whose planes' rows are 16 but not 32 bytes apart: the texture header reads such a plane through a pool image,
/// which the copy engine fills from the plane before the turn copy that loads it (raw_turned_pass.cpp). Boot 21 ran them raw
/// with 1906 to 5000 pixels off in each, the 96x64 versions over 256-byte rows exact.
/// wrong_r<turn>_<cell>: pixels of the target and the second plane off Metal's store (0 to pass); paths: the command
/// buffers' paths (raw/N to pass on the raw facade).
struct TurnedUnalignedPlanesTest: HWTest {
    let id = "gpu.raw.turned_unaligned_planes"
    let name = "Turned planes of 16-byte rows loaded beside a turned target hold Metal's texels"
    var subsystem: Subsystem { .metal }
    var timeout: TimeInterval { 90 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let raw = RawFacade.kind(of: gpu.device) == "raw"
        let states = try RotatedStates(gpu)
        var metrics = Metrics()
        var failures: [String] = []
        for turn in 1...4 {
            let cells = RotatedCells(gpu: gpu, states: states, turn: turn, width: 90, height: 60, aligned: false)
            guard let plane = try cells.makePlane() else {
                return .fail("the device makes no texture with the rotation SPI", metrics, proof: gpu.proof)
            }
            for cell in [RotatedCell.besideR4, .turns, .frames] {
                let key = "r\(turn)_\(cell.rawValue)"
                let result = try await cells.run(cell, into: plane)
                metrics.set("wrong_\(key)", result.wrong)
                if result.wrong != 0 { failures.append("\(key): \(result.wrong) pixels off") }
                if raw && result.path != "raw" { failures.append("\(key) ran \(result.path)") }
            }
        }
        metrics.set("paths", gpu.paths.text)
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}
