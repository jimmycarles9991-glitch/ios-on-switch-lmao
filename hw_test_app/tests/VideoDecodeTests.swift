import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// One test clip through VTDecompressionSession with RequireHardwareAcceleratedVideoDecoder, every frame checked
/// against the pattern.
struct VideoDecodeTest: HWTest {
    let clip: Clip
    let subsystem = Subsystem.decode
    var id: String { "decode.\(clip.name)" }
    var name: String { "\(Self.codecName(clip.codec)) \(clip.label) (\(clip.frames) frames)" }
    var timeout: TimeInterval { 90 }

    static func codecName(_ codec: VideoCodec) -> String {
        switch codec {
        case .h264: return "H.264"
        case .hevc: return "HEVC Main"
        case .hevc10: return "HEVC Main10"
        case .vp9: return "VP9"
        }
    }

    var codecType: CMVideoCodecType {
        switch clip.codec {
        case .h264: return kCMVideoCodecType_H264
        case .hevc, .hevc10: return kCMVideoCodecType_HEVC
        case .vp9: return kCMVideoCodecType_VP9
        }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        // iOS 26.2 offers VP9 only to processes that opt in; before it, only to entitled ones.
        if clip.codec == .vp9, #available(iOS 26.2, *) {
            VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9)
        }
        guard VTIsHardwareDecodeSupported(codecType) else {
            if clip.codec == .vp9 { return .skip("VTIsHardwareDecodeSupported(VP9) is false") }
            return .fail("VTIsHardwareDecodeSupported is false: no hardware \(Self.codecName(clip.codec)) decoder")
        }
        let demuxed = try await Demux.load(clip, from: context.media)
        let result: DecodeResult
        do {
            result = try Decompressor.decode(
                demuxed.samples, format: demuxed.format, pattern: clip.pattern, bitDepth: clip.codec.bitDepth,
                fps: clip.fps)
        } catch VideoError.status(let call, let status) where clip.height >= 2160 && Self.unsupported.contains(status) {
            return .skip("the hardware decoder does not take \(clip.label): \(call) \(status)")
        }
        var metrics = Metrics()
        metrics.set("frames", result.frames)
        metrics.set("matched", result.matched)
        metrics.set("ms_per_frame", result.millisecondsPerFrame, digits: 2)
        metrics.set("fps", result.fps, digits: 1)
        metrics.set("luma_err", result.worstLumaError, digits: 1)
        metrics.set("chroma_err", result.worstChromaError, digits: 1)
        return .judged(
            [
                ("UsingHardwareAcceleratedVideoDecoder", result.usingHardware),
                ("every frame decodes to the pattern (\(result.firstMismatch ?? "ok"))", result.matched == clip.frames),
            ], metrics, proof: result.usingHardware ? "UsingHardwareAcceleratedVideoDecoder=1" : "")
    }

    static let unsupported: Set<OSStatus> = [
        kVTVideoDecoderUnsupportedDataFormatErr, kVTCouldNotFindVideoDecoderErr, kVTVideoDecoderNotAvailableNowErr,
    ]

    static var all: [VideoDecodeTest] { Clip.all.map { VideoDecodeTest(clip: $0) } }
}
