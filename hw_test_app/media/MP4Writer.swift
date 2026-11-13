import AVFoundation
import CoreMedia
import Foundation
import HWTestVideo

/// Writes compressed samples to an MP4 without re-encoding. Two things vary between runs on the same input and are
/// taken out so the same frames always make the same file: the SEI messages Apple's encoders put in the first frame,
/// and the creation and modification times AVAssetWriter stamps into mvhd, tkhd and mdhd.
enum MP4Writer {
    static func write(_ clip: CompressedClip, to url: URL) throws {
        try write(clip.samples, format: clip.format, to: url)
    }

    static func write(_ compressed: [CMSampleBuffer], format: CMVideoFormatDescription, to url: URL) throws {
        try? FileManager.default.removeItem(at: url)
        let samples = try compressed.map { try withoutSEI($0, hevc: format.mediaSubType != .h264) }
        let writer = try AVAssetWriter(outputURL: url, fileType: .mp4)
        let input = AVAssetWriterInput(mediaType: .video, outputSettings: nil, sourceFormatHint: format)
        input.expectsMediaDataInRealTime = false
        writer.add(input)
        guard writer.startWriting() else {
            throw VideoError.message("AVAssetWriter: \(String(describing: writer.error))")
        }
        writer.startSession(atSourceTime: .zero)
        let done = DispatchSemaphore(value: 0)
        let queue = DispatchQueue(label: "media.mp4")
        var next = 0
        input.requestMediaDataWhenReady(on: queue) {
            while input.isReadyForMoreMediaData && next < samples.count {
                input.append(samples[next])
                next += 1
            }
            if next == samples.count {
                input.markAsFinished()
                writer.finishWriting { done.signal() }
            }
        }
        done.wait()
        guard writer.status == .completed else {
            throw VideoError.message("AVAssetWriter: \(String(describing: writer.error))")
        }
        var data = try Data(contentsOf: url)
        zeroTimes(&data)
        try data.write(to: url)
    }

    /// The sample with its SEI NAL units dropped (AVCC/HVCC framing: 4-byte big-endian lengths).
    static func withoutSEI(_ sample: CMSampleBuffer, hevc: Bool) throws -> CMSampleBuffer {
        guard let block = CMSampleBufferGetDataBuffer(sample) else { return sample }
        let length = CMBlockBufferGetDataLength(block)
        var bytes = [UInt8](repeating: 0, count: length)
        guard CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: &bytes) == noErr else {
            throw VideoError.message("CMBlockBufferCopyDataBytes failed")
        }
        var kept = Data()
        var at = 0
        while at + 4 <= length {
            let size = Int(bytes[at]) << 24 | Int(bytes[at + 1]) << 16 | Int(bytes[at + 2]) << 8 | Int(bytes[at + 3])
            guard size > 0, at + 4 + size <= length else { break }
            let type = hevc ? Int(bytes[at + 4] >> 1 & 0x3F) : Int(bytes[at + 4] & 0x1F)
            let sei = hevc ? (type == 39 || type == 40) : type == 6
            if !sei { kept.append(contentsOf: bytes[at..<(at + 4 + size)]) }
            at += 4 + size
        }
        guard kept.count != length else { return sample }
        return try Demux.sample(
            kept, format: CMSampleBufferGetFormatDescription(sample)!, pts: sample.presentationTimeStamp,
            duration: sample.duration, sync: sample.isKeyFrame)
    }

    static let containers: Set<String> = ["moov", "trak", "mdia"]
    static let stamped: Set<String> = ["mvhd", "tkhd", "mdhd"]

    static func zeroTimes(_ data: inout Data, from start: Int = 0, to end: Int? = nil) {
        var at = start
        let end = end ?? data.count
        while at + 8 <= end {
            var size = (0..<4).reduce(0) { $0 << 8 | Int(data[at + $1]) }
            let type = String(decoding: data[(at + 4)..<(at + 8)], as: UTF8.self)
            if size == 1, at + 16 <= end {
                size = (8..<16).reduce(0) { $0 << 8 | Int(data[at + $1]) }  // 64-bit largesize (mdat)
            } else if size == 0 {
                size = end - at
            }
            guard size >= 8, at + size <= end else { return }
            if containers.contains(type) {
                zeroTimes(&data, from: at + 8, to: at + size)
            } else if stamped.contains(type) {
                // Full box: version byte, 3 flag bytes, then creation and modification times (32-bit each in
                // version 0, 64-bit in version 1).
                let width = data[at + 8] == 1 ? 8 : 4
                data.replaceSubrange((at + 12)..<(at + 12 + 2 * width), with: [UInt8](repeating: 0, count: 2 * width))
            }
            at += size
        }
    }
}
