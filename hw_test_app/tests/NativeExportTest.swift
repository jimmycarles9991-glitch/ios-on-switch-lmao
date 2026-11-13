import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// The 8-bit H.264 720p clip decoded once, and the decoder's own count of how each picture reached the client
/// (SwitchPictureRoutes, debug builds of the AVD plugin): exported as the destination surface's native NVDEC picture,
/// placed by VIC, or copied by the CPU. LIMITS V1: every 720p, 1080p and 4K session exported none. claim_verdict is
/// PASS when every picture went out natively; INCONCLUSIVE when the decoder does not say.
/// decode.native_export_main10 is the same count for HEVC Main 10: the decoder's P010 picture goes out as the 'x420'
/// surface's two 16-bit planes (LIMITS V12), where every Main 10 picture used to be a VIC copy.
/// decode.native_export_main10_8bit asks for '420v' from the Main 10 clip: NVDEC writes each picture's 8-bit display
/// buffer in the decode job (disp_output_mode 2, V13) and that buffer goes out natively; matched counts the pictures
/// equal to the pattern, so a reference written at 8 bits (drift in the P pictures) fails it.
/// All three also hold the kernel to a fixed set of buffers (L964, from a debug kernel): the decoder's recycles take
/// back parked buffers and published pictures reuse retired surface objects, so allocations and new objects stay at
/// the pool's depth while recycles and publications count every picture.
struct NativeExportTest: HWTest {
    let id: String
    let name: String
    let codec: VideoCodec
    let probe: String
    var outputBitDepth = 0
    let subsystem = Subsystem.decode
    var timeout: TimeInterval { 60 }

    static let h264 = NativeExportTest(
        id: "decode.native_export",
        name: "H.264 720p: pictures exported natively against VIC copies, by the decoder's count", codec: .h264,
        probe: "V1")
    static let main10 = NativeExportTest(
        id: "decode.native_export_main10",
        name: "HEVC Main 10 720p: P010 pictures exported natively against VIC copies, by the decoder's count",
        codec: .hevc10, probe: "V12")
    static let main10To8 = NativeExportTest(
        id: "decode.native_export_main10_8bit",
        name: "HEVC Main 10 720p into '420v': NVDEC's 8-bit display buffers exported natively, by the decoder's count",
        codec: .hevc10, probe: "V13", outputBitDepth: 8)

    /// SwitchNVDECPark's capacity and the retired-publication ring's (16 each). A session that allocated a decoder
    /// buffer or a surface object per picture passes it within the clip; one cycling a fixed set stays at its pool's
    /// depth.
    static let poolBound = 16
    static let poolKeys = [
        "recycles", "recycles_replaced", "recycles_parked", "buffers_allocated", "native_exports", "native_objects",
        "parked_most",
    ]

    /// Why the kernel's counts for the session break the fixed-set claim, "" when they keep it, nil from a release
    /// kernel, which does not count.
    static func poolProblem(_ routes: [String: Int]) -> String? {
        guard let allocated = routes["buffers_allocated"], let objects = routes["native_objects"],
            let exports = routes["native_exports"], let recycles = routes["recycles"]
        else { return nil }
        let exported = routes["exported"] ?? 0
        if allocated > poolBound { return "\(allocated) decoder buffers allocated, past the pool's \(poolBound)" }
        if objects > poolBound { return "\(objects) new surface objects, past the pool's \(poolBound)" }
        if exports != exported { return "the kernel published \(exports) of the \(exported) exported pictures" }
        if recycles + poolBound < exported { return "\(recycles) recycles for \(exported) pictures" }
        return ""
    }

    func run(_ context: TestContext) async throws -> Outcome {
        guard let clip = Clip.all.first(where: { $0.codec == codec && $0.height == 720 }) else {
            return .skip("no \(codec) 720p clip")
        }
        let demuxed = try await Demux.load(clip, from: context.media)
        let result = try Decompressor.decode(
            demuxed.samples, format: demuxed.format, pattern: clip.pattern,
            bitDepth: outputBitDepth > 0 ? outputBitDepth : clip.codec.bitDepth,
            fps: clip.fps)
        var metrics = Metrics()
        metrics.set("frames", result.frames)
        metrics.set("matched", result.matched)
        let finding: ProbeFinding
        if let routes = result.routes {
            for key in ["exported", "vic", "cpu", "errors"] { metrics.set("decoder_" + key, routes[key] ?? 0) }
            for key in Self.poolKeys { if let value = routes[key] { metrics.set(key, value) } }
            let notNative = (routes["vic"] ?? 0) + (routes["cpu"] ?? 0)
            let wrong = result.frames - result.matched
            let pool = Self.poolProblem(routes)
            let broken = notNative != 0 || (routes["exported"] ?? 0) == 0 || wrong != 0 || pool?.isEmpty == false
            finding = ProbeFinding(
                broken ? .fail : pool == nil ? .inconclusive : .pass, metrics,
                note: notNative != 0
                    ? "\(notNative) pictures were copied, not exported"
                    : wrong != 0
                        ? "\(wrong) pictures differ from the pattern"
                        : pool ?? "the kernel does not count the session's buffers (a release kernel)")
        } else {
            finding = ProbeFinding(.inconclusive, metrics, note: "the decoder does not answer SwitchPictureRoutes")
        }
        var base = Metrics()
        base.set("hardware", result.usingHardware)
        return finding.outcome(probe: probe, base: base, proof: "")
    }
}
