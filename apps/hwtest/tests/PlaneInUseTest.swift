import AVFoundation
import CoreMedia
import CoreVideo
import HWTestCore
import IOSurface
import UIKit

/// A decoder's pool reuses a pixel buffer once IOSurfaceIsInUse says nothing holds its surface. Three 1280x720 '420v'
/// IOSurface buffers go round an AVSampleBufferDisplayLayer at 30 Hz for two seconds, the way a decoder pool feeds a
/// video layer, while Core Animation detaches the layer onto window B. The buffer shown last has to read in use while
/// it is on screen (shown_in_use), or a pool could rewrite it while window B scans it; window B has to have flipped
/// (kern.switch_planes_*, debug kernels), or the check covers the GPU composite rather than the plane.
@MainActor
final class PlaneInUseRun {
    let layer = AVSampleBufferDisplayLayer()
    private(set) var buffers: [CVPixelBuffer] = []
    private var format: CMVideoFormatDescription?

    init() {
        let attributes: [CFString: Any] = [
            kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
            kCVPixelBufferWidthKey: 1280, kCVPixelBufferHeightKey: 720,
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        ]
        for index in 0..<3 {
            var buffer: CVPixelBuffer?
            CVPixelBufferCreate(
                nil, 1280, 720, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes as CFDictionary, &buffer)
            guard let buffer else { continue }
            CVBufferSetAttachment(
                buffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, .shouldPropagate)
            CVPixelBufferLockBaseAddress(buffer, [])
            for plane in 0..<2 {
                guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { continue }
                let rows = CVPixelBufferGetHeightOfPlane(buffer, plane)
                let bytes = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane) * rows
                memset(base, plane == 0 ? Int32(64 + index * 48) : 128, bytes)
            }
            CVPixelBufferUnlockBaseAddress(buffer, [])
            buffers.append(buffer)
        }
        if let first = buffers.first {
            CMVideoFormatDescriptionCreateForImageBuffer(
                allocator: nil, imageBuffer: first, formatDescriptionOut: &format)
        }
        layer.videoGravity = .resizeAspect
    }

    func enqueue(_ frame: Int) {
        guard let format, !buffers.isEmpty else { return }
        var timing = CMSampleTimingInfo(
            duration: CMTime(value: 1, timescale: 30),
            presentationTimeStamp: CMTime(value: Int64(frame), timescale: 30), decodeTimeStamp: .invalid)
        var sample: CMSampleBuffer?
        CMSampleBufferCreateReadyWithImageBuffer(
            allocator: nil, imageBuffer: buffers[frame % buffers.count], formatDescription: format,
            sampleTiming: &timing, sampleBufferOut: &sample)
        guard let sample else { return }
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true),
            CFArrayGetCount(attachments) > 0
        {
            let dictionary = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(
                dictionary, Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }
        layer.sampleBufferRenderer.enqueue(sample)
    }

    func inUse(_ index: Int) -> Bool {
        guard index < buffers.count, let surface = CVPixelBufferGetIOSurface(buffers[index]) else { return false }
        return IOSurfaceIsInUse(surface.takeUnretainedValue())
    }
}

struct PlaneInUseTest: HWTest {
    let id = "display.plane_in_use"
    let name = "A surface on window B reads in use while the display scans it"
    let subsystem = Subsystem.compositor

    func run(_ context: TestContext) async throws -> Outcome {
        let (before, unread) = PlanesTest.totals()
        guard !before.isEmpty else {
            var metrics = Metrics()
            metrics.set("sysctl", KernelSysctl.code(unread))
            return .skip(KernelSysctl.unreadable("kern.switch_planes_*", unread), metrics)
        }
        let run = await context.onStage(fullScreen: true) { stage -> PlaneInUseRun in
            let run = PlaneInUseRun()
            run.layer.frame = stage.bounds
            stage.layer.addSublayer(run.layer)
            return run
        }
        let frames = 60
        for frame in 0..<frames {
            await MainActor.run { run.enqueue(frame) }
            try await Task.sleep(nanoseconds: 33_000_000)
        }
        try await Task.sleep(nanoseconds: 300_000_000)
        let shown = (frames - 1) % 3
        let (count, shownInUse, othersInUse) = await MainActor.run {
            (run.buffers.count, run.inUse(shown), (0..<3).filter { $0 != shown && run.inUse($0) }.count)
        }
        let after = PlanesTest.totals().values
        let delta = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }

        var metrics = Metrics()
        metrics.set("buffers", count)
        metrics.set("shown_in_use", shownInUse ? 1 : 0)
        metrics.set("others_in_use", othersInUse)
        for name in PlanesTest.counters { metrics.set("d_" + name, delta(name)) }
        return .judged(
            [
                ("three IOSurface buffers", count == 3),
                ("window B flipped (d_flips > 0)", delta("flips") > 0),
                ("the buffer on screen reads in use (shown_in_use 1)", shownInUse),
            ], metrics)
    }
}
