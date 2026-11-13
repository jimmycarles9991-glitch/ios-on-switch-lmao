import CoreVideo
import Foundation
import HWTestCore
import HWTestVideo
import VideoToolbox

/// VTPixelTransferSession from 1080p NV12 into a 960x540 '422v': the 4:2:2 biplanar output TegraVIC writes for Core
/// Animation's detached-video copies on iPadOS 27, unturned and 2:1 so filtering adds little. VideoToolbox may convert
/// it itself instead of asking the scaler; TegraVIC's stats line in the kernel log shows whether VIC took the job.
struct PixelTransfer422Test: HWTest {
    let id = "image.pixel_transfer_422"
    let name = "VTPixelTransferSession into 4:2:2 biplanar"
    let subsystem = Subsystem.images

    func run(_ context: TestContext) async throws -> Outcome {
        var session: VTPixelTransferSession?
        guard VTPixelTransferSessionCreate(allocator: nil, pixelTransferSessionOut: &session) == noErr, let session
        else {
            return .fail("VTPixelTransferSessionCreate failed")
        }
        defer { VTPixelTransferSessionInvalidate(session) }
        let clip: UInt16 = 0x71C
        let source = try PatternPixels.make(
            Pattern(width: 1920, height: 1080, clip: clip), frame: 5, bitDepth: 8, pool: nil)
        let (width, height) = (960, 540)
        let format = kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange
        var destination: CVPixelBuffer?
        let attributes = PatternPixels.attributes(width: width, height: height, format: format)
        guard
            CVPixelBufferCreate(nil, width, height, format, attributes as CFDictionary, &destination)
                == kCVReturnSuccess,
            let destination
        else { return .fail("CVPixelBufferCreate \(width)x\(height) 422v") }
        let start = CFAbsoluteTimeGetCurrent()
        let status = VTPixelTransferSessionTransferImage(session, from: source, to: destination)
        let milliseconds = (CFAbsoluteTimeGetCurrent() - start) * 1000
        guard status == noErr else { return .fail("VTPixelTransferSessionTransferImage: \(status)") }
        let check = PatternPixels.check(
            destination, pattern: Pattern(width: width, height: height, clip: clip), expected: 5)
        var metrics = Metrics()
        metrics.set("convert_1080p_422v_ms", milliseconds, digits: 2)
        metrics.set("convert_err", max(check.lumaError, check.chromaError), digits: 1)
        return .judged(
            [("1080p NV12 -> 960x540 422v keeps the pattern", check.matches(frame: 5, tolerance: 12))], metrics)
    }
}
