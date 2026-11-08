import CoreVideo
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// VTPixelTransferSession, which TegraVIC carries: 4K NV12 scaled to 1080p NV12, and 1080p NV12 scaled and
/// converted to 720p BGRA. The pattern is defined by position relative to the frame, so a scaled copy is the same
/// pattern at the smaller size.
struct PixelTransferTest: HWTest {
    let id = "image.pixel_transfer"
    let name = "VTPixelTransferSession scale and convert"
    let subsystem = Subsystem.images
    static let runs = 30

    func transfer(
        _ session: VTPixelTransferSession, from source: CVPixelBuffer, width: Int, height: Int, format: OSType
    ) throws -> (CVPixelBuffer, Double) {
        var destination: CVPixelBuffer?
        let attributes = PatternPixels.attributes(width: width, height: height, format: format)
        guard
            CVPixelBufferCreate(nil, width, height, format, attributes as CFDictionary, &destination)
                == kCVReturnSuccess,
            let destination
        else { throw TestFailure("CVPixelBufferCreate \(width)x\(height)") }
        let start = CFAbsoluteTimeGetCurrent()
        for _ in 0..<Self.runs {
            let status = VTPixelTransferSessionTransferImage(session, from: source, to: destination)
            guard status == noErr else { throw TestFailure("VTPixelTransferSessionTransferImage: \(status)") }
        }
        return (destination, (CFAbsoluteTimeGetCurrent() - start) * 1000 / Double(Self.runs))
    }

    func run(_ context: TestContext) async throws -> Outcome {
        var session: VTPixelTransferSession?
        guard VTPixelTransferSessionCreate(allocator: nil, pixelTransferSessionOut: &session) == noErr, let session
        else {
            return .fail("VTPixelTransferSessionCreate failed")
        }
        defer { VTPixelTransferSessionInvalidate(session) }
        let clip: UInt16 = 0x71C
        let uhd = try PatternPixels.make(
            Pattern(width: 3840, height: 2160, clip: clip), frame: 5, bitDepth: 8, pool: nil)
        let hd = try PatternPixels.make(
            Pattern(width: 1920, height: 1080, clip: clip), frame: 5, bitDepth: 8, pool: nil)
        // Pattern.rgb is BT.601; tag the source so the conversion uses the same matrix.
        CVBufferSetAttachment(hd, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4, .shouldPropagate)
        let (scaled, scaleMs) = try transfer(
            session, from: uhd, width: 1920, height: 1080, format: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
        let (converted, convertMs) = try transfer(
            session, from: hd, width: 1280, height: 720, format: kCVPixelFormatType_32BGRA)
        let scaledCheck = PatternPixels.check(
            scaled, pattern: Pattern(width: 1920, height: 1080, clip: clip), expected: 5)
        let convertedCheck = PatternPixels.checkBGRA(
            converted, pattern: Pattern(width: 1280, height: 720, clip: clip), expected: 5)
        var metrics = Metrics()
        metrics.set("scale_4k_1080p_ms", scaleMs, digits: 2)
        metrics.set("convert_1080p_720p_bgra_ms", convertMs, digits: 2)
        metrics.set("scale_err", max(scaledCheck.lumaError, scaledCheck.chromaError), digits: 1)
        metrics.set("convert_err", convertedCheck.lumaError, digits: 1)
        return .judged(
            [
                ("4K -> 1080p NV12 keeps the pattern", scaledCheck.matches(frame: 5, tolerance: 12)),
                ("1080p NV12 -> 720p BGRA keeps the pattern", convertedCheck.matches(frame: 5, tolerance: 16)),
            ], metrics)
    }
}
