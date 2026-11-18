import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// The bundled HEVC Main 10 clip decoded with the AVD plugin's display-buffer probe asked for (debug builds): its first
/// IDR picture decoded again in disp_output_mode 2 and 3 with NVDEC's display buffer bound, each compared with the
/// mode-1 picture's 10 bits truncated to 8, and mode 3 also with three Rec.2020 -> Rec.709 conversions of it (gamma: a
/// 3x3 on R'G'B', linear: on linear light, matrix: the matrices only). mode3_model names the closest; AVD sends Rec.2020
/// SDR streams to 8-bit clients through mode 3 tagged Rec.709 (HEVCNVDEC.h hevc_display_mode_for). claim_verdict PASS:
/// mode 2 within one code of the truncation and mode 3 within one code of a conversion everywhere. INCONCLUSIVE when the
/// decoder does not say.
struct DisplayBufferTest: HWTest {
    let id = "decode.display_buffer"
    let name = "HEVC Main 10: NVDEC's 8-bit display buffer in modes 2 and 3 against the 10-bit picture"
    let subsystem = Subsystem.decode
    var timeout: TimeInterval { 60 }
    static let models = ["gamma", "linear", "matrix"]

    func run(_ context: TestContext) async throws -> Outcome {
        guard let clip = Clip.all.first(where: { $0.codec == .hevc10 }) else {
            return .skip("no HEVC Main 10 clip")
        }
        let demuxed = try await Demux.load(clip, from: context.media)
        let result = try Decompressor.decode(
            demuxed.samples, format: demuxed.format, pattern: clip.pattern, bitDepth: clip.codec.bitDepth,
            fps: clip.fps, probeDisplayBuffer: true)
        var metrics = Metrics()
        metrics.set("frames", result.frames)
        let finding: ProbeFinding
        if let probe = result.displayBufferProbe {
            for (key, value) in probe.sorted(by: { $0.key < $1.key }) { metrics.set(key, value) }
            let written = [2, 3].allSatisfy { probe["mode\($0)_status"] == 0 && (probe["mode\($0)_layout"] ?? 0) != 0 }
            let closest = Self.models.min { (probe["mode3_\($0)_max"] ?? 255) < (probe["mode3_\($1)_max"] ?? 255) }
            let closestMax = closest.flatMap { probe["mode3_\($0)_max"] } ?? 255
            metrics.set("mode3_model", closestMax <= 1 ? closest ?? "none" : "none")
            let close = written && (probe["mode2_max"] ?? 255) <= 1 && closestMax <= 1
            finding = ProbeFinding(close ? .pass : .fail, metrics, note: close ? "" : "see the mode fields")
        } else {
            finding = ProbeFinding(.inconclusive, metrics, note: "the decoder does not answer SwitchDisplayBufferProbe")
        }
        var base = Metrics()
        base.set("hardware", result.usingHardware)
        return finding.outcome(probe: "V14", base: base, proof: "")
    }
}
