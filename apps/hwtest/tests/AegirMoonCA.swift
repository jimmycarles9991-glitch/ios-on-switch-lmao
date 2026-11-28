import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.gm20b.aegir_moon's Core Animation arm: Weather's moon destOut state, drawn as gpu.ca.sweep draws a corpus state
/// (CAStateSweep) and held to the same state drawn by this IPSW's QuartzCore on a Mac (the bundle's ca_goldens.txt).
/// The state is the state lane's 25 Nov harvest (precompile/corpus.log, ca-names/24B119.txt), its pack record
/// e04837c84abb50c7; a bundle without it in ca_states.log names that instead of passing.
enum AegirMoonCA {
    static let label = "com.apple.coreanimation.PBGRABaddXmw_TcimA3Xhf_Ialp"
    static let sweepKey = "3290523f0993ff0a"

    struct Reading {
        var verdict = "no_state"
        var packHit: Bool?
        var path = "-"
    }

    static func run(_ gpu: GPU) async -> Reading {
        var reading = Reading()
        guard let statesURL = Bundle.main.url(forResource: "ca_states", withExtension: "log"),
              let text = try? String(contentsOf: statesURL, encoding: .utf8),
              let goldensURL = Bundle.main.url(forResource: "ca_goldens", withExtension: "txt"),
              let goldenText = try? String(contentsOf: goldensURL, encoding: .utf8)
        else {
            reading.verdict = "no_bundle_corpus"
            return reading
        }
        guard let line = CAStateSweep.states(text, everyState: true).first(where: { $0.label == label }) else {
            return reading
        }
        guard CAStateSweep.key(line) == sweepKey else {
            reading.verdict = "other_line:\(CAStateSweep.key(line))"
            return reading
        }
        do {
            let library = try gpu.device.makeLibrary(URL: CAState.library)
            reading.packHit = GM20BFacade.packHit(try CAState.pipeline(line, library: library, device: gpu.device))
            let drawn = try await CAStateSweep.draw(line, library: library, gpu: gpu)
            reading.path = drawn.path
            reading.verdict = judged(line, drawn, CAGoldens(goldenText))
        } catch {
            reading.verdict = "failed:" + String(describing: error).prefix(60)
        }
        return reading
    }

    /// exact, close (every 8x8 block within one code of the Mac's), coarse (close, some block further), wrong, or
    /// no_golden, as gpu.ca.sweep judges a state
    static func judged(_ line: PipelineLine, _ drawn: CAStateSweep.Drawn, _ goldens: CAGoldens) -> String {
        guard let golden = goldens.states[sweepKey] else { return "no_golden" }
        var verdict = "exact"
        for (color, bytes) in zip(line.colors.sorted(by: { $0.index < $1.index }), drawn.attachments) {
            let ours = Signature(
                bytes: bytes, metalFormat: color.format, width: CAStateSweep.width, height: CAStateSweep.height
            )
            switch GoldenVerdict.of(ours, against: golden[color.index], metalFormat: color.format) {
            case .exact: continue
            case .close:
                if let reference = golden[color.index],
                   !CAGoldens.withinOneCode(ours, against: reference, metalFormat: color.format)
                {
                    verdict = "coarse"
                } else if verdict == "exact" {
                    verdict = "close"
                }
            case .missing: return "no_golden"
            case .malformed: return "malformed_golden"
            case .wrong: return "wrong"
            }
        }
        return verdict
    }
}
