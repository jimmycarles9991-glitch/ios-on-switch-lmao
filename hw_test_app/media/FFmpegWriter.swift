import Foundation
import HWTestCore

/// The MPEG-4 Part 2, MPEG-2, MPEG-1 and MBAFF H.264 clips, from FFmpeg's encoders (`ffmpeg` on PATH, or $FFMPEG): the
/// Mac's VideoToolbox encodes none of them. The pattern goes in as raw I420 frames; one thread keeps the bytes the same from
/// run to run. The MP4 files describe MPEG-2 and MPEG-1 by objectTypeIndication (0x61, 0x6a), which CoreMedia turns
/// into 'mp2v' and 'mp1v'.
///   MPEG-4 720p: Simple profile (I- and P-VOPs, H.263 quantisation). 1080p: Advanced Simple (two B-VOPs between
///     references, quarter-sample motion, MPEG quantisation with both matrices loaded), a stream NVDEC takes that the
///     Mac's software decoder refuses (codecBadDataErr).
///   MPEG-2 720p: Main profile, progressive, two B-pictures, default matrices. 1080p: interlaced coding (field DCT and
///     field prediction in frame pictures), both matrices loaded, alternate scan, intra VLC table, non-linear quantiser
///     scale, 10-bit intra DC.
///   MPEG-1 720p: two B-pictures, a loaded intra matrix.
///   H.264 MBAFF 1080: libx264's interlaced coding (macroblock-adaptive frame/field, top field first), High profile,
///     two B-pictures.
enum FFmpegWriter {
    struct Failure: Error, CustomStringConvertible {
        let description: String
    }

    static let intraMatrix = [
        8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27,
        29,
        34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27,
        29,
        35, 38, 46, 56, 69, 83,
    ]
    static let interMatrix = [
        16, 17, 18, 19, 20, 21, 22, 23, 17, 18, 19, 20, 21, 22, 23, 24, 18, 19, 20, 21, 22, 23, 24, 25, 19, 20, 21, 22,
        23,
        24, 26, 27, 20, 21, 22, 23, 25, 26, 27, 28, 21, 22, 23, 24, 26, 27, 28, 30, 22, 23, 24, 26, 27, 28, 30, 31, 23,
        24,
        25, 27, 28, 30, 31, 40,
    ]

    static func encoder(_ clip: Clip) throws -> [String] {
        let matrix = { (values: [Int]) in values.map(String.init).joined(separator: ",") }
        let rate = ["-threads", "1", "-g", "30", "-b:v", "\(clip.bitrate)"]
        switch clip.codec {
        case .mpeg4 where clip.height <= 720:
            return ["-c:v", "mpeg4", "-bf", "0"] + rate
        case .mpeg4:
            return [
                "-c:v", "mpeg4", "-bf", "2", "-flags", "+qpel", "-mpeg_quant", "1", "-intra_matrix",
                matrix(intraMatrix), "-inter_matrix", matrix(interMatrix),
            ] + rate
        case .mpeg2 where clip.height <= 720:
            return ["-c:v", "mpeg2video", "-bf", "2"] + rate
        case .mpeg2:
            return [
                "-c:v", "mpeg2video", "-bf", "2", "-flags", "+ildct+ilme", "-intra_matrix", matrix(intraMatrix),
                "-inter_matrix", matrix(interMatrix), "-alternate_scan", "1", "-intra_vlc", "1",
                "-non_linear_quant", "1", "-qmax", "28", "-dc", "10",
            ] + rate
        case .mpeg1:
            return ["-c:v", "mpeg1video", "-bf", "2", "-intra_matrix", matrix(interMatrix)] + rate
        case .h264Mbaff:
            return [
                "-c:v", "libx264", "-profile:v", "high", "-flags", "+ildct+ilme", "-x264-params", "tff=1", "-bf", "2",
            ] + rate
        default:
            throw Failure(description: "\(clip.file): not an FFmpeg clip")
        }
    }

    static func write(_ clip: Clip, to url: URL) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        let ffmpeg = ProcessInfo.processInfo.environment["FFMPEG"] ?? "ffmpeg"
        process.arguments =
            [
                ffmpeg, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s",
                "\(clip.width)x\(clip.height)",
                "-r", "\(clip.fps)", "-i", "-",
            ] + (try encoder(clip)) + ["-f", "mp4", url.path]
        let input = Pipe()
        process.standardInput = input
        try process.run()
        let lumaBytes = clip.width * clip.height
        var frame = Data(count: lumaBytes * 3 / 2)
        let pattern = clip.pattern
        for index in 0..<clip.frames {
            frame.withUnsafeMutableBytes { bytes in
                let base = bytes.baseAddress!
                PatternWriter.writeLuma(pattern, frame: index, into: .init(base: base, rowBytes: clip.width))
                PatternWriter.writeChromaPlanar(
                    pattern, cb: .init(base: base + lumaBytes, rowBytes: clip.width / 2),
                    cr: .init(base: base + lumaBytes * 5 / 4, rowBytes: clip.width / 2))
            }
            try input.fileHandleForWriting.write(contentsOf: frame)
        }
        try input.fileHandleForWriting.close()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            throw Failure(description: "\(clip.file): \(ffmpeg) exited \(process.terminationStatus)")
        }
    }
}
