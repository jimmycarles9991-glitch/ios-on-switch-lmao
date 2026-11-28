import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import HWTestKit
import Metal

/// A 550 over a 'b3a8' whose rows are off 32 bytes (gpu/XrPlanesRows.swift), against an M4 Max: made; a draw reading
/// it into RGBA16Float writes each
/// texel's (code - 384) / 510 and alpha byte / 255 within one half step, after the fill and after a blit of new words
/// (filled_off, blitted_off 0 of 4,096; 3,744 and 2,915 of them not the nearest halves on the M4 Max, reported as
/// *_inexact); a kernel writing every texel back with alpha 1 leaves plane 0 the blitted words and plane 1 all 255
/// (words_off, alpha_off 0).
struct XrPlanesRowsOff32Test: HWTest {
    let id = "gpu.gm20b.xr_planes_rows_off32"
    let name = "An XR texture over planes whose rows are off 32 bytes reads, takes a blit and a kernel store, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 45 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let r = try await XrPlanesRows.observe(gpu)
            metrics.set("made", r.made)
            guard r.made else { return [("no texture over the planes", false)] }
            let counts = [
                ("filled_off", r.filledOff), ("blitted_off", r.blittedOff), ("words_off", r.wordsOff),
                ("alpha_off", r.alphaOff),
            ]
            for (key, value) in counts + [("filled_inexact", r.filledInexact), ("blitted_inexact", r.blittedInexact)] {
                metrics.set(key, value)
            }
            metrics.set("first_off", r.first)
            return counts.map { ("\($0.0)=\($0.1)", $0.1 == 0) }
        }
    }
}

/// 550 and 551 targets over one plane alone (gpu/XrWrittenOnePlane.swift), each against an M4 Max: a Clear (0.5, 0.25,
/// -0.25, 1) stores every word 0x27f80100 (550) or 0x2f7a486e (551) and nothing past the rows (past_changed 0); a Load
/// pass blending that colour with source factor OneMinusDestinationAlpha stores it as over alpha 0 (0x27f80100); the
/// target read by a draw is that word's colour with alpha 0 (texels_off 0, within one half step); a 16x16 550 over
/// plane 1 of a 'b3a8' stores 0x27f80100. <arm>_word: the first word, <arm>_words_off: words not it.
struct XrWrittenOnePlaneTest: HWTest {
    let id = "gpu.gm20b.xr_written_one_plane"
    let name = "XR targets over a plane with no alpha plane clear, blend and read as over alpha 0, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 45 }

    static let words: [XrWrittenOnePlane.Arm: UInt32] = [.clear551: 0x2f7a_486e]

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            for arm in XrWrittenOnePlane.Arm.allCases {
                let r = try await XrWrittenOnePlane.observe(gpu, arm), key = arm.rawValue
                let want = Self.words[arm] ?? 0x27f8_0100
                guard r.made else {
                    checks.append(("\(key): no texture", false))
                    continue
                }
                metrics.set(key + "_word", "0x" + String(r.word, radix: 16))
                metrics.set(key + "_words_off", r.wordsOff)
                metrics.set(key + "_past_changed", r.pastChanged)
                if arm == .read550 { metrics.set(key + "_texels_off", r.texelsOff) }
                let holds = r.word == want && r.wordsOff == 0 && r.pastChanged == 0 && r.texelsOff == 0
                checks.append((
                    "\(key): word 0x\(String(r.word, radix: 16)) (\(r.paths.joined(separator: ",")))",
                    holds
                ))
            }
            return checks
        }
    }
}
