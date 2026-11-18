import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// Which input layouts NVENC 5.0 reads, decided on the unit: the AVE plug-in's SwitchEncoderLayoutProbe (debug builds)
/// codes one 720p pattern picture again as an IDR from each layout (the CPU writing it in that layout, and VIC writing
/// BL2, the route the encoder ships, and BL16), and each IDR is decoded back here against the pattern. The firmware turns
/// block_height into a code (TegraNVENC/nvidia/SOURCE): 4, 8, 16 GOBs are codes 1-3, while 0 (pitch) and 2 GOBs share
/// code 0, so exactly one of "pitch" and "bl2" can decode back. NVIDIA's driver sends its own 2-GOB surfaces as code 0,
/// so "bl2" is expected; "pitch" reading would mean NVENC can take an IOSurface without VIC.
struct NVENCInputLayoutTest: HWTest {
    let id = "encode.nvenc_input_layouts"
    let name = "NVENC input layouts: one IDR from each, decoded back"
    let subsystem = Subsystem.encode
    var timeout: TimeInterval { 60 }

    static let layouts = ["tiled16", "pitch", "bl2", "bl4", "bl8", "bl16", "vic_bl2", "vic_bl16"]

    func run(_ context: TestContext) async throws -> Outcome {
        let (width, height, fps) = (1280, 720, 30)
        let pattern = Pattern(width: width, height: height, clip: 0x7004)
        let settings = CompressorSettings(
            codec: .h264, width: width, height: height, fps: fps, bitrate: 8_000_000, realTime: false,
            requireHardware: true)
        var probe: [String: Any]?
        let encoded = try Compressor.encode(settings, pattern: pattern, frames: 1) { session in
            probe = Compressor.read(session, "SwitchEncoderLayoutProbe" as CFString) as? [String: Any]
        }
        var metrics = Metrics()
        guard let probe else {
            return .fail("no SwitchEncoderLayoutProbe: the encoder is not a debug build of the AVE plug-in", metrics)
        }
        var reads: [String: Bool] = [:]
        for layout in Self.layouts {
            guard let data = probe[layout] as? Data else {
                metrics.set(layout, "failed \((probe[layout] as? NSNumber)?.intValue ?? 0)")
                continue
            }
            let decoded = try Decompressor.decode(
                [try Self.sample(data, format: encoded.format, fps: fps)], format: encoded.format, pattern: pattern,
                bitDepth: 8, fps: fps)
            reads[layout] = decoded.matched == 1
            metrics.set(layout, decoded.matched == 1 ? "reads" : "wrong")
            metrics.set("\(layout)_luma_err", decoded.worstLumaError, digits: 1)
        }
        let codeZero = reads["pitch"] == true ? "pitch" : reads["bl2"] == true ? "bl2" : "neither"
        metrics.set("code0_reads", codeZero)
        return .judged(
            [
                ("16x16 tiles, the layout E3 proved, decode back", reads["tiled16"] == true),
                (
                    "code 0 reads exactly one of pitch rows and 2-GOB blocks",
                    (reads["pitch"] == true) != (reads["bl2"] == true)
                ),
                (
                    "4, 8 and 16 GOBs a block (codes 1-3) decode back",
                    ["bl4", "bl8", "bl16"].allSatisfy { reads[$0] == true }
                ),
                ("VIC's BL2, the encoder's input route, decodes back", reads["vic_bl2"] == true),
                ("VIC's BL16 decodes back", reads["vic_bl16"] == true),
            ], metrics, proof: "code0=\(codeZero)")
    }

    /// One AVCC IDR of the session's stream as a sample at frame 0.
    static func sample(_ data: Data, format: CMVideoFormatDescription, fps: Int) throws -> CMSampleBuffer {
        var block: CMBlockBuffer?
        var status = CMBlockBufferCreateWithMemoryBlock(
            allocator: nil, memoryBlock: nil, blockLength: data.count, blockAllocator: nil, customBlockSource: nil,
            offsetToData: 0, dataLength: data.count, flags: kCMBlockBufferAssureMemoryNowFlag, blockBufferOut: &block)
        guard status == noErr, let block else { throw VideoError.status("CMBlockBufferCreateWithMemoryBlock", status) }
        status = data.withUnsafeBytes {
            CMBlockBufferReplaceDataBytes(
                with: $0.baseAddress!, blockBuffer: block, offsetIntoDestination: 0, dataLength: data.count)
        }
        guard status == noErr else { throw VideoError.status("CMBlockBufferReplaceDataBytes", status) }
        var timing = CMSampleTimingInfo(
            duration: CMTime(value: 1, timescale: CMTimeScale(fps)), presentationTimeStamp: .zero,
            decodeTimeStamp: .zero)
        var size = data.count
        var sample: CMSampleBuffer?
        status = CMSampleBufferCreateReady(
            allocator: nil, dataBuffer: block, formatDescription: format, sampleCount: 1, sampleTimingEntryCount: 1,
            sampleTimingArray: &timing, sampleSizeEntryCount: 1, sampleSizeArray: &size, sampleBufferOut: &sample)
        guard status == noErr, let sample else { throw VideoError.status("CMSampleBufferCreateReady", status) }
        return sample
    }
}
