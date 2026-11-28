import Foundation
import HWTestCore
import Metal

/// gpu.perf.first_touch: the A/B arms that push state this driver never pushed on the chip before, each a possible
/// ILLEGAL_MTHD or a channel the GPU stops: subtiling knob 0x06164010 (uam commented it out; gpu.perf.knobs' arms and
/// its golden write-back after them), the Z_4X2 zcull region format and DISCARD_RENDER_TARGET (gpu.perf.nvn's arms).
/// Both apps run it after every other test, so a broken channel can mislead no test after it; discard runs last of
/// the three.
extension GPURates {
    public static func firstTouch(_ gpu: GPU, _ s: Sizes = .device) async throws -> SceneResult {
        let b = GPURates(gpu, s)
        try await b.alu()
        var notes: [String] = []
        if b.has("switchMetalProbeSubtilingKnob:") {
            await b.knobArms(lastKnob, try b.subtileKit())
        } else {
            notes.append("the device has no switchMetalProbeSubtilingKnob: probe: no 0x06164010 arms")
        }
        if !b.has("switchMetalProbeNvn:") {
            notes.append("the device has no switchMetalProbeNvn: probe: each arm's on form is its off form")
        }
        await b.isolated("zcull_4x2") { try await b.zcullFormat("zcull_4x2", Self.nvnZcull4x2) }
        await b.isolated("discard") { try await b.discard() }
        var r = SceneResult()
        r.metrics = b.metrics
        r.notes = notes
        for p in b.problems { r.expect(false, p) }
        return r
    }
}
