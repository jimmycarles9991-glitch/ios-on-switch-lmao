import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A Core Animation state the driver draws on rung 3 (the ROP's iterated blend) drawn again in a later pass: in the
/// same command buffer, in the next one, after a pass that clears another texture, in a pass that clears first, after a
/// blit, after a compute encoder and after a pass of another state (gpu/IteratedBlendPasses.swift). Whatever sets the
/// blend back between two draws of the state must also forget its bound segment, or the later draw runs with the blend
/// off: magenta where QuartzCore's multiply of magenta over the clear belongs. The blit's copy engine meets no blend,
/// so its case judges the pass after it; the compute encoder runs with the blend still on, so its words are judged too.
/// Each case reads colour 0 back against the CPU's multiply and the work between against its own result. Per case:
/// <case>_wrong= colour 0's pixels off it (and <case>_first= the first as x,y:B/G/R/A), <case>_between_wrong= the work
/// between's texels or words off, <case>_rung3= the facade's rung-3 draws of the case's draws ("-" without the
/// counter), <case>_paths=. single is the control: one pass, one draw, right on every build; a single that fails is the
/// model or the form, not a pass boundary. Every command buffer runs with its error tolerated, so a class error that
/// kills the channel fails its case and the next case runs.
struct IteratedBlendPassesTest: HWTest {
    let id = "gpu.gm20b.iterated_blend_passes"
    let name = "A rung-3 Core Animation state drawn again after a pass end, a command buffer or other work, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        guard let url = Bundle.main.url(forResource: "ca_states", withExtension: "log"),
              let text = try? String(contentsOf: url, encoding: .utf8)
        else { return .fail("ca_states.log is not in the app's bundle") }
        guard
            let line = CAStateSweep.states(text, everyState: true).first(where: {
                $0.shortLabel == IteratedBlendPasses.label
            })
        else { return .skip("this QuartzCore makes no \(IteratedBlendPasses.label)") }
        let scene = try IteratedBlendPasses(
            line: line, library: try gpu.device.makeLibrary(URL: CAState.library), gpu: gpu
        )
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for between in IteratedBlendPasses.Between.allCases {
            let drawn = try await scene.run(between)
            let key = between.rawValue
            let raw = drawn.paths.allSatisfy { $0 == "gm20b" }
            let rung3 = drawn.rung3.map { $0 >= drawn.draws } ?? true
            metrics.set("\(key)_wrong", drawn.wrong)
            if drawn.wrong > 0 { metrics.set("\(key)_first", drawn.first) }
            if let wrong = drawn.between { metrics.set("\(key)_between_wrong", wrong) }
            metrics.set("\(key)_rung3", drawn.rung3.map { "\($0)/\(drawn.draws)" } ?? "-")
            metrics.set("\(key)_paths", raw ? "gm20b" : String(drawn.paths.joined(separator: ",").prefix(80)))
            checks.append(("\(key): colour 0 is the CPU's multiply", drawn.wrong == 0))
            checks.append(("\(key): the work between kept its own result", (drawn.between ?? 0) == 0))
            checks.append(("\(key): every draw of the state on rung 3", rung3))
            checks.append(("\(key): every command buffer raw, none failed", raw))
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
