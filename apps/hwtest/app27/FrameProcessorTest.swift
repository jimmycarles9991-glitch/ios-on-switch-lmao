import CoreMedia
import CoreVideo
import Foundation
import HWTestCore
import HWTestKit
import VideoToolbox

/// VideoToolbox's frame processors (iOS 26): which of them the OS offers on this device (one 0/1 field each), and,
/// when the temporal noise filter is offered, one 1280x720 frame through it with the previous and next frames it asks
/// for. Its output must arrive without an error and must not be black where the input is mid-grey. SKIP when none is
/// offered.
struct FrameProcessorTest: HWTest {
    let id = "video.frame_processor"
    let name = "VideoToolbox frame processors: support and one noise-filtered frame"
    let subsystem = Subsystem.decode

    static let (width, height) = (1280, 720)

    func run(_ context: TestContext) async throws -> Outcome {
        let offered: [(String, Bool)] = [
            ("frc", VTFrameRateConversionConfiguration.isSupported),
            ("optical_flow", VTOpticalFlowConfiguration.isSupported),
            ("motion_blur", VTMotionBlurConfiguration.isSupported),
            ("super_res", VTSuperResolutionScalerConfiguration.isSupported),
            ("ll_super_res", VTLowLatencySuperResolutionScalerConfiguration.isSupported),
            ("ll_interpolation", VTLowLatencyFrameInterpolationConfiguration.isSupported),
            ("noise_filter", VTTemporalNoiseFilterConfiguration.isSupported),
        ]
        var metrics = Metrics()
        for (name, supported) in offered { metrics.set(name, supported) }
        guard offered.contains(where: \.1) else { return .skip("no frame processor offered", metrics) }
        guard VTTemporalNoiseFilterConfiguration.isSupported else {
            return .pass(metrics, detail: "the noise filter is not offered: support reported only")
        }
        guard
            let configuration = VTTemporalNoiseFilterConfiguration(
                frameWidth: Self.width, frameHeight: Self.height,
                sourcePixelFormat: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
        else { return .fail("the noise filter refused 1280x720 420v", metrics) }
        let processor = VTFrameProcessor()
        try processor.startSession(configuration: configuration)
        defer { processor.endSession() }
        let (previous, next) = (configuration.previousFrameCount ?? 0, configuration.nextFrameCount ?? 0)
        let frames = try (0...(previous + next)).map { index in
            try Self.frame(Self.greyBuffer(configuration.sourcePixelBufferAttributes), index)
        }
        let destination = try Self.frame(
            Self.greyBuffer(configuration.destinationPixelBufferAttributes, luma: 0), previous)
        guard
            let parameters = VTTemporalNoiseFilterParameters(
                sourceFrame: frames[previous], nextFrames: Array(frames.suffix(next)),
                previousFrames: Array(frames.prefix(previous)), destinationFrame: destination, filterStrength: 0.5,
                hasDiscontinuity: false)
        else { return .fail("the noise filter's parameters were refused", metrics) }
        let start = CFAbsoluteTimeGetCurrent()
        try await processor.process(parameters: parameters)
        metrics.set("filter_ms", (CFAbsoluteTimeGetCurrent() - start) * 1000, digits: 1)
        let luma = Self.meanLuma(destination.buffer)
        metrics.set("out_luma", luma)
        return .judged([("the filtered grey frame is not black (luma > 64)", luma > 64)], metrics)
    }

    static func frame(_ buffer: CVPixelBuffer, _ index: Int) throws -> VTFrameProcessorFrame {
        let time = CMTime(value: CMTimeValue(index), timescale: 60)
        guard let frame = VTFrameProcessorFrame(buffer: buffer, presentationTimeStamp: time) else {
            throw TestFailure("VTFrameProcessorFrame refused a \(width)x\(height) buffer")
        }
        return frame
    }

    /// A 420v buffer of the processor's attributes, its luma `luma` and its chroma neutral.
    static func greyBuffer(_ attributes: [String: Any], luma: UInt8 = 128) throws -> CVPixelBuffer {
        var buffer: CVPixelBuffer?
        let status = CVPixelBufferCreate(
            nil, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes as CFDictionary, &buffer)
        guard status == kCVReturnSuccess, let buffer else { throw TestFailure("CVPixelBufferCreate: \(status)") }
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        for (plane, value) in [(0, luma), (1, UInt8(128))] {
            let rows = CVPixelBufferGetHeightOfPlane(buffer, plane)
            let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane)
            memset(base, Int32(value), CVPixelBufferGetBytesPerRowOfPlane(buffer, plane) * rows)
        }
        return buffer
    }

    static func meanLuma(_ buffer: CVPixelBuffer) -> Int {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, 0) else { return 0 }
        let rowBytes = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0)
        let rows = base.assumingMemoryBound(to: UInt8.self)
        var total = 0
        for y in stride(from: 0, to: height, by: 8) {
            for x in stride(from: 0, to: width, by: 8) { total += Int(rows[y * rowBytes + x]) }
        }
        return total / ((height / 8) * (width / 8))
    }
}
