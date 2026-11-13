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
        case .mpeg4: return "MPEG-4 Part 2"
        case .mpeg2: return "MPEG-2"
        case .mpeg1: return "MPEG-1"
        case .h264Paff: return "H.264 PAFF"
        case .h264Mbaff: return "H.264 MBAFF"
        }
    }

    var codecType: CMVideoCodecType {
        switch clip.codec {
        case .h264, .h264Paff, .h264Mbaff: return kCMVideoCodecType_H264
        case .hevc, .hevc10: return kCMVideoCodecType_HEVC
        case .vp9: return kCMVideoCodecType_VP9
        case .mpeg4: return kCMVideoCodecType_MPEG4Video
        case .mpeg2: return kCMVideoCodecType_MPEG2Video
        case .mpeg1: return kCMVideoCodecType_MPEG1Video
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
        var checks = [
            ("UsingHardwareAcceleratedVideoDecoder", result.usingHardware),
            ("every frame decodes to the pattern (\(result.firstMismatch ?? "ok"))", result.matched == clip.frames),
        ]
        var proof = result.usingHardware ? "UsingHardwareAcceleratedVideoDecoder=1" : ""
        // Debug builds of the AVD plug-in count the pictures its NVDEC jobs put out, how each reached the client, where
        // its decode thread waited, and how long from DecodeFrame to each picture's emission (decode.async_emit judges
        // the waits).
        if let routes = result.routes {
            for key in DecodeRouteTest.keys {
                metrics.set("decoder_" + key, routes[key] ?? 0)
            }
            checks.append(
                (
                    "every frame an NVDEC picture (SwitchPictureRoutes)",
                    routes["frames"] == clip.frames && routes["errors"] == 0
                ))
            proof += " SwitchPictureRoutes frames=\(routes["frames"] ?? 0)"
        }
        return .judged(checks, metrics, proof: proof)
    }

    static let unsupported: Set<OSStatus> = [
        kVTVideoDecoderUnsupportedDataFormatErr, kVTCouldNotFindVideoDecoderErr, kVTVideoDecoderNotAvailableNowErr,
    ]

    static var all: [VideoDecodeTest] { Clip.all.map { VideoDecodeTest(clip: $0) } }
}
