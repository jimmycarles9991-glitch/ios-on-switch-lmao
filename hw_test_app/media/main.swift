import CryptoKit
import Foundation
import HWTestCore
import HWTestVideo
import ImageIO
import VideoToolbox

/// The test media's generator, run by hand: the build takes the files pinned in bazel/pinned/hw_test_app/media.
///
///     media_tool <out dir>        writes every clip and image of HWTestCore's tables, and prints their SHA256SUMS lines
///     media_tool --verify <dir>   decodes every clip there on this Mac's decoders and checks it against the pattern
func run(_ arguments: [String]) async throws {
    if arguments.first == "--verify", arguments.count == 2 {
        try await verify(URL(fileURLWithPath: arguments[1]))
        return
    }
    guard arguments.count == 1 else { throw VideoError.message("usage: media_tool <out dir> | --verify <dir>") }
    let out = URL(fileURLWithPath: arguments[0])
    try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
    for clip in Clip.all {
        let url = out.appendingPathComponent(clip.file)
        if clip.codec == .vp9 {
            try VP9Writer.write(clip, to: url)
        } else if [.mpeg4, .mpeg2, .mpeg1, .h264Mbaff].contains(clip.codec) {
            try FFmpegWriter.write(clip, to: url)
        } else if clip.codec == .h264Paff {
            try H264FieldWriter.write(clip, to: url)
        } else {
            // The Mac's hardware encoder spreads a 4K frame over however many encode engines are free, so a 4K clip's
            // bytes differ from run to run; the frames it decodes to do not. That is why the clips are pinned.
            let compressed = try Compressor.encode(
                CompressorSettings(clip, requireHardware: false), pattern: clip.pattern, frames: clip.frames)
            try MP4Writer.write(compressed, to: url)
        }
        try printPin(url)
    }
    let alpha = out.appendingPathComponent(AlphaClip.file)
    try MP4Writer.write(try AlphaVideo.encode(), to: alpha)
    try printPin(alpha)
    for image in StillImage.all {
        let url = out.appendingPathComponent(image.file)
        try StillWriter.write(image, to: url)
        try printPin(url)
    }
}

func printPin(_ url: URL) throws {
    let digest = SHA256.hash(data: try Data(contentsOf: url)).map { String(format: "%02x", $0) }.joined()
    print("\(digest)  hw_test_app/media/\(url.lastPathComponent)")
}

/// Any decoder the Mac has will do here (VP9 is a supplemental software decoder on macOS): this checks the files,
/// not the hardware.
func verify(_ directory: URL) async throws {
    VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9)
    for clip in Clip.all where FileManager.default.fileExists(atPath: directory.appendingPathComponent(clip.file).path)
    {
        let demuxed = try await Demux.load(clip, from: directory)
        do {
            let result = try Decompressor.decode(
                demuxed.samples, format: demuxed.format, pattern: clip.pattern, bitDepth: clip.codec.bitDepth,
                fps: clip.fps, requireHardware: false)
            print(
                clip.file, "\(result.matched)/\(clip.frames) frames match", result.firstMismatch ?? "",
                String(format: "luma %.1f chroma %.1f", result.worstLumaError, result.worstChromaError))
        } catch {
            print(clip.file, "not decoded here: \(error)")
        }
    }
    let alpha = directory.appendingPathComponent(AlphaClip.file)
    if FileManager.default.fileExists(atPath: alpha.path) {
        let demuxed = try await Demux.mp4(alpha)
        let result = try AlphaVideo.decode(
            demuxed.samples, format: demuxed.format,
            flags: [._EnableAsynchronousDecompression, ._EnableTemporalProcessing])
        print(
            AlphaClip.file, "\(result.matched)/\(AlphaClip.frames) frames match", result.firstMismatch ?? "",
            "\(result.outOfDisplayOrder) out of display order",
            String(format: "luma %.1f chroma %.1f alpha %.1f", result.worstLuma, result.worstChroma, result.worstAlpha))
    }
    for image in StillImage.all {
        let url = directory.appendingPathComponent(image.file)
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
            let decoded = CGImageSourceCreateImageAtIndex(source, 0, nil)
        else { continue }
        let context = CGContext(
            data: nil, width: decoded.width, height: decoded.height, bitsPerComponent: 8,
            bytesPerRow: decoded.width * 4,
            space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
        context.draw(decoded, in: CGRect(x: 0, y: 0, width: decoded.width, height: decoded.height))
        let base = context.data!.assumingMemoryBound(to: UInt8.self)
        let result = PatternChecker(image.pattern).checkRGB { region in
            var sum = (0.0, 0.0, 0.0)
            for y in region.y..<(region.y + region.height) {
                for x in region.x..<(region.x + region.width) {
                    let at = (y * decoded.width + x) * 4
                    sum = (sum.0 + Double(base[at]), sum.1 + Double(base[at + 1]), sum.2 + Double(base[at + 2]))
                }
            }
            let count = Double(region.width * region.height)
            return (sum.0 / count, sum.1 / count, sum.2 / count)
        }
        print(image.file, result.matches(frame: 0, tolerance: image.tolerance) ? "matches" : "MISMATCH", result)
    }
}

do {
    try await run(Array(CommandLine.arguments.dropFirst()))
} catch {
    FileHandle.standardError.write(Data("media_tool: \(error)\n".utf8))
    exit(1)
}
