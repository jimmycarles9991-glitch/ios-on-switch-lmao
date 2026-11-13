import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// 1080p60 pattern frames through a hardware-only H.264 session that allows frame reordering (not real time): AVE codes
/// two B pictures between anchors (AVEReorder.h). The samples must come out in decode order with the B pictures as
/// non-reference B slices and decode timestamps the presentation times in order; fed in that order to the hardware
/// decoder, every frame must come back matching the pattern at its presentation time. display_order says whether the
/// decoder put them out in display order: AVD puts frames out as they are decoded and clients order them by
/// presentation time (AVDDecoder.c kReorder), as Apple's decoders do on a Mac with this test's flags. A held frame is
/// coded from its own IOSurface when its turn comes, by VIC like every other (SwitchEncoderRoutes).
struct VideoEncodeBFramesTest: HWTest {
    let id = "encode.h264_bframes"
    let name = "H.264 encode with B pictures (frame reordering)"
    let subsystem = Subsystem.encode
    var timeout: TimeInterval { 90 }
    static let frames = 60
    /// IDR, then 19 groups of B B P, then the last two frames as B P when the session completes.
    static let bPictures = 39

    /// The slice type (H.264 7.4.3: 0 P, 1 B, 2 I) of the sample's first NAL unit and its nal_ref_idc.
    static func slice(_ sample: CMSampleBuffer) -> (type: Int, reference: Bool)? {
        guard let block = CMSampleBufferGetDataBuffer(sample) else { return nil }
        var bytes = [UInt8](repeating: 0, count: min(CMBlockBufferGetDataLength(block), 32))
        guard bytes.count > 6,
            CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: bytes.count, destination: &bytes) == noErr
        else { return nil }
        var bit = 5 * 8
        func ue() -> Int {
            var zeros = 0
            while bit < bytes.count * 8, bytes[bit / 8] >> (7 - bit % 8) & 1 == 0 {
                zeros += 1
                bit += 1
            }
            bit += 1
            var value = 0
            for _ in 0..<zeros where bit < bytes.count * 8 {
                value = value << 1 | Int(bytes[bit / 8] >> (7 - bit % 8) & 1)
                bit += 1
            }
            return (1 << zeros) - 1 + value
        }
        _ = ue()  // first_mb_in_slice
        return (ue() % 5, bytes[4] & 0x60 != 0)
    }

    static func attachment(_ sample: CMSampleBuffer, _ key: CFString) -> Bool? {
        guard
            let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false)
                as? [[CFString: Any]]
        else { return nil }
        return attachments.first?[key] as? Bool
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let (width, height, fps) = (1920, 1080, 60)
        let pattern = Pattern(width: width, height: height, clip: 0x7004)
        var settings = CompressorSettings(
            codec: .h264, width: width, height: height, fps: fps, bitrate: 12_000_000, realTime: false,
            requireHardware: true)
        settings.allowReordering = true
        let encoded = try Compressor.encode(settings, pattern: pattern, frames: Self.frames)
        let slices = encoded.decodeOrder.map(Self.slice)
        let bSamples = encoded.decodeOrder.filter {
            Self.attachment($0, kCMSampleAttachmentKey_IsDependedOnByOthers) == false
                && Self.attachment($0, kCMSampleAttachmentKey_NotSync) == true
        }
        let bSlices = slices.filter { $0?.type == 1 && $0?.reference == false }.count
        let bMarked = bSlices == Self.bPictures && bSamples.count == Self.bPictures
        let decodeTimes = encoded.decodeOrder.map(\.decodeTimeStamp)
        let shown = encoded.samples.map(\.presentationTimeStamp)
        let rising = zip(decodeTimes, decodeTimes.dropFirst()).allSatisfy { $0 < $1 }
        let reordered = zip(decodeTimes, encoded.decodeOrder.map(\.presentationTimeStamp)).filter { $0 != $1 }.count
        let decoded = try Decompressor.decode(
            encoded.decodeOrder, format: encoded.format, pattern: pattern, bitDepth: 8, fps: fps, temporal: true)
        var metrics = Metrics()
        metrics.set("fps", Double(Self.frames) / encoded.seconds, digits: 1)
        metrics.set("mbps", Double(encoded.bytes * 8) / (Double(Self.frames) / Double(fps)) / 1e6, digits: 2)
        metrics.set("key_frames", encoded.keyFrames)
        metrics.set("b_frames", bSlices)
        metrics.set("p_frames", slices.filter { $0?.type == 0 }.count)
        metrics.set("dts_reordered", reordered)
        metrics.set("decoded_match", decoded.matched)
        metrics.set("display_order", decoded.displayOrdered)
        metrics.set("luma_err", decoded.worstLumaError, digits: 1)
        metrics.set("input_vic", encoded.routes?["input_vic"] ?? -1)
        metrics.set("input_bounced", encoded.routes?["input_bounced"] ?? -1)
        metrics.set("input_cpu_copies", encoded.routes?["input_cpu_copies"] ?? -1)
        metrics.set("input_refused", encoded.routes?["input_refused"] ?? -1)
        return .judged(
            [
                ("UsingHardwareAcceleratedVideoEncoder", encoded.usingHardware),
                ("\(Self.bPictures) non-reference B slices, each sample marked so", bMarked),
                ("one key frame", encoded.keyFrames == 1),
                ("decode times rise and are the presentation times in order", rising && decodeTimes == shown),
                ("decode and presentation times differ", reordered > 0),
                ("decodes back to the pattern (\(decoded.firstMismatch ?? "ok"))", decoded.matched == Self.frames),
                (
                    "every input, held frames' too, written by VIC from its own surface, none copied or refused",
                    VideoEncodeTest.allByVIC(encoded.routes, Self.frames)
                ),
            ], metrics, proof: encoded.usingHardware ? "encoder=\(encoded.encoderID)" : "")
    }
}
