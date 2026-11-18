import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import VideoToolbox

/// VIC reading a surface with a writer promise (drivers/TegraGM20B/SwitchGpuWriterPromise.h) orders behind it on the
/// GPU: the job's dependencies carry the promise (promise_waits), which the GPU-to-host1x bridge acquires before VIC's
/// syncpoint wait lets the job read, with no CPU sleep for it. The test's own connection promises an NV12 surface's
/// writes with no job to keep them, as gpu.path.promise_cancel does, and a VTPixelTransferSession scale (TegraVIC)
/// reads the surface; 30 ms later the test cancels the promise. The transfer must return only after the cancel, with
/// the surface's CPU fill.
struct PromiseVICPathTest: GPUPathTest {
    let id = "gpu.path.promise_vic"
    let name = "A VIC transfer of a surface with a writer promise waits for it on the GPU"
    let counters = ["writer_promises", "promise_waits", "promise_cpu_releases"]
    static let width = 256, height = 256, luma: UInt8 = 200, chroma: UInt8 = 128
    static let parkMs = 30.0, goesOnMs = 150.0

    static func nv12(_ width: Int, _ height: Int) throws -> CVPixelBuffer {
        var buffer: CVPixelBuffer?
        let attributes: [String: Any] = [kCVPixelBufferIOSurfacePropertiesKey as String: [:]]
        guard
            CVPixelBufferCreate(
                nil, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes as CFDictionary,
                &buffer) == kCVReturnSuccess, let buffer
        else { throw TestFailure("no \(width)x\(height) NV12 pixel buffer") }
        return buffer
    }

    static func fill(_ buffer: CVPixelBuffer) {
        CVPixelBufferLockBaseAddress(buffer, [])
        for (plane, value) in [(0, luma), (1, chroma)] {
            let rows = CVPixelBufferGetHeightOfPlane(buffer, plane)
            let rowBytes = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
            if let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) {
                memset(base, Int32(value), rows * rowBytes)
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, [])
    }

    /// The scaled luma's samples off the source's fill (VIC's filter keeps a flat plane flat)
    static func wrongLuma(_ buffer: CVPixelBuffer) -> Int {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, 0) else { return -1 }
        let rowBytes = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0)
        var wrong = 0
        for row in 0..<CVPixelBufferGetHeightOfPlane(buffer, 0) {
            let line = (base + row * rowBytes).assumingMemoryBound(to: UInt8.self)
            for x in 0..<CVPixelBufferGetWidthOfPlane(buffer, 0) where abs(Int(line[x]) - Int(luma)) > 2 {
                wrong += 1
            }
        }
        return wrong
    }

    func exercise(_ gpu: GPU, _ context: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        guard PathParts.ownRouteRaw(gpu, part: "the test", counters: counters, &metrics) else { return [] }
        guard let maker = GPUConnection() else { throw TestFailure("no TegraGM20B connection") }
        let source = try Self.nv12(Self.width, Self.height)
        let destination = try Self.nv12(Self.width / 2, Self.height / 2)
        Self.fill(source)
        guard let backing = CVPixelBufferGetIOSurface(source)?.takeUnretainedValue() else {
            throw TestFailure("the source has no IOSurface")
        }
        var session: VTPixelTransferSession?
        guard VTPixelTransferSessionCreate(allocator: nil, pixelTransferSessionOut: &session) == noErr, let session
        else { throw TestFailure("VTPixelTransferSessionCreate failed") }
        defer { VTPixelTransferSessionInvalidate(session) }

        let (handle, promise) = try PromiseCancelPathTest.promise(unsafeBitCast(backing, to: IOSurface.self), maker)
        defer {
            _ = maker.call(
                PromiseCancelPathTest.boFree, GPUConnection.words(PromiseCancelPathTest.halves(handle)), outputSize: 0)
        }
        let start = DispatchTime.now()
        let cancel = TransferCancel()
        DispatchQueue.global().asyncAfter(deadline: start + Self.parkMs / 1000) {
            cancel.at = DispatchTime.now()
            let words = GPUConnection.words([0, 0, PromiseCancelPathTest.promiseCancel, 0, promise, 0])
            cancel.result = maker.call(PromiseCancelPathTest.writerPromise, words, outputSize: 8).0
            cancel.done.signal()
        }
        let status = VTPixelTransferSessionTransferImage(session, from: source, to: destination)
        let ended = DispatchTime.now()
        cancel.done.wait()
        let tookMs = Double(ended.uptimeNanoseconds - start.uptimeNanoseconds) / 1e6
        let afterMs = Double(Int64(ended.uptimeNanoseconds) - Int64(cancel.at.uptimeNanoseconds)) / 1e6
        let wrong = Self.wrongLuma(destination)
        metrics.set("took_ms", String(format: "%.1f", tookMs))
        metrics.set("after_cancel_ms", String(format: "%.1f", afterMs))
        metrics.set("wrong_luma", wrong)
        var failures: [String] = []
        if status != noErr { failures.append("VTPixelTransferSessionTransferImage: \(status)") }
        if cancel.result != KERN_SUCCESS {
            failures.append("the cancel was refused: \(GPUConnection.hex(cancel.result))")
        }
        if tookMs < Self.parkMs * 0.8 {
            failures.append(String(format: "the transfer returned %.1f ms in: it never waited for the promise", tookMs))
        }
        if afterMs > Self.goesOnMs {
            failures.append(
                String(format: "the transfer returned %.1f ms after the cancel (at most %.0f)", afterMs, Self.goesOnMs))
        }
        if wrong != 0 { failures.append("\(wrong) scaled luma samples are off the surface's fill \(Self.luma)") }
        return failures
    }
}

private final class TransferCancel: @unchecked Sendable {
    var at = DispatchTime.now(), result = KERN_SUCCESS
    let done = DispatchSemaphore(value: 0)
}
