import AVFoundation
import CoreMedia
import Foundation
import HWTestCore

public struct Demuxed {
    public let format: CMVideoFormatDescription
    public let samples: [CMSampleBuffer]
}

/// Compressed samples out of the test clips, untouched, for feeding VTDecompressionSession directly.
public enum Demux {
    public static func load(_ clip: Clip, from directory: URL) async throws -> Demuxed {
        let url = directory.appendingPathComponent(clip.file)
        return clip.codec == .vp9 ? try ivf(Data(contentsOf: url), clip: clip) : try await mp4(url)
    }

    public static func mp4(_ url: URL) async throws -> Demuxed {
        let asset = AVURLAsset(url: url)
        guard let track = try await asset.loadTracks(withMediaType: .video).first else {
            throw VideoError.message("\(url.lastPathComponent) has no video track")
        }
        guard let format = try await track.load(.formatDescriptions).first else {
            throw VideoError.message("\(url.lastPathComponent) has no format description")
        }
        let reader = try AVAssetReader(asset: asset)
        let output = AVAssetReaderTrackOutput(track: track, outputSettings: nil)
        reader.add(output)
        guard reader.startReading() else {
            throw VideoError.message("AVAssetReader: \(reader.error.map(String.init(describing:)) ?? "did not start")")
        }
        var samples: [CMSampleBuffer] = []
        while let sample = output.copyNextSampleBuffer() {
            if CMSampleBufferGetNumSamples(sample) > 0 { samples.append(sample) }
        }
        guard reader.status == .completed else {
            throw VideoError.message("AVAssetReader: \(reader.error.map(String.init(describing:)) ?? "incomplete")")
        }
        return Demuxed(format: format, samples: samples)
    }

    public static func ivf(_ data: Data, clip: Clip) throws -> Demuxed {
        let (header, frames) = try IVF.parse(data)
        guard header.fourcc == "VP90", header.width == clip.width, header.height == clip.height else {
            throw VideoError.message("\(clip.file): \(header.fourcc) \(header.width)x\(header.height)")
        }
        let format = try vp9Format(clip)
        let samples = try frames.map { frame in
            try sample(
                frame.data, format: format,
                pts: CMTime(value: CMTimeValue(frame.pts), timescale: CMTimeScale(clip.fps)),
                duration: CMTime(value: 1, timescale: CMTimeScale(clip.fps)))
        }
        return Demuxed(format: format, samples: samples)
    }

    /// The VP9 sample description VideoToolbox wants: a VPCodecConfigurationRecord ('vpcC', version 1) for profile 0,
    /// 8-bit 4:2:0, BT.709, studio range, the level from the clip's luma sample rate.
    static func vp9Format(_ clip: Clip) throws -> CMVideoFormatDescription {
        let rate = clip.width * clip.height * clip.fps
        let level: UInt8 = rate <= 36_864_000 ? 31 : rate <= 83_558_400 ? 40 : rate <= 160_432_128 ? 41 : 50
        let record: [UInt8] = [1, 0, 0, 0, 0, level, 8 << 4 | 1 << 1, 1, 1, 1, 0, 0]
        let extensions: [CFString: Any] = [
            kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms: ["vpcC": Data(record)]
        ]
        var format: CMVideoFormatDescription?
        let status = CMVideoFormatDescriptionCreate(
            allocator: nil, codecType: kCMVideoCodecType_VP9, width: Int32(clip.width), height: Int32(clip.height),
            extensions: extensions as CFDictionary, formatDescriptionOut: &format)
        guard status == noErr, let format else { throw VideoError.status("CMVideoFormatDescriptionCreate", status) }
        return format
    }

    /// One compressed frame as a sample buffer; `sync: false` marks it a non-key frame (kCMSampleAttachmentKey_NotSync).
    public static func sample(
        _ data: Data, format: CMFormatDescription, pts: CMTime, duration: CMTime, sync: Bool = true
    ) throws -> CMSampleBuffer {
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
        var timing = CMSampleTimingInfo(duration: duration, presentationTimeStamp: pts, decodeTimeStamp: .invalid)
        var size = data.count
        var sample: CMSampleBuffer?
        status = CMSampleBufferCreateReady(
            allocator: nil, dataBuffer: block, formatDescription: format, sampleCount: 1, sampleTimingEntryCount: 1,
            sampleTimingArray: &timing, sampleSizeEntryCount: 1, sampleSizeArray: &size, sampleBufferOut: &sample)
        guard status == noErr, let sample else { throw VideoError.status("CMSampleBufferCreateReady", status) }
        if !sync, let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true) {
            let first = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(
                first, Unmanaged.passUnretained(kCMSampleAttachmentKey_NotSync).toOpaque(),
                Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }
        return sample
    }
}
