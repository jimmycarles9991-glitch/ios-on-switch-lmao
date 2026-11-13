import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A GPU reader parked on a writer promise (drivers/TegraGM20B/SwitchGpuWriterPromise.h) goes on once the CPU writes
/// the promise's word. The test's own connection imports an IOSurface and promises its writes (TegraGM20BJobABI.h
/// method 34) with no job to keep the promise; a raw command buffer of the Metal device, another connection, copies the
/// surface into a buffer, so its job acquires the promise's fence word on the GPU (ACQ_GEQ) and parks there. 30 ms
/// later the test cancels the promise, which the kernel writes from the CPU. The copy must complete soon after, with
/// the surface's CPU fill: a channel parked on ACQ_GEQ reads the word again after a CPU write, which the kernel's
/// cancel, kill and close releases and the give-up's raise all rely on. Were it not so, the copy would wait for the
/// give-up a second after it parked (its "writer promise ... was not kept" line).
struct PromiseCancelPathTest: GPUPathTest {
    let id = "gpu.path.promise_cancel"
    let name = "A job parked on the GPU for a writer promise goes on once the CPU cancels it"
    let counters = ["writer_promises", "promise_waits", "promise_cpu_releases"]
    static let size = 64, parkMs = 30.0, goesOnMs = 150.0
    static let fill: [UInt8] = [255, 0, 0, 255]  // BGRA blue
    static let boImport: UInt32 = 3, boFree: UInt32 = 4, writerPromise: UInt32 = 34
    static let surfaceWrite: UInt32 = 1 << 1, promiseMake: UInt32 = 0, promiseCancel: UInt32 = 1

    static func halves(_ value: UInt64) -> [UInt32] { [UInt32(value & 0xffff_ffff), UInt32(value >> 32)] }

    /// The surface imported into `maker` and its writes promised: the import's handle and the promise.
    static func promise(_ surface: IOSurface, _ maker: GPUConnection) throws -> (UInt64, UInt32) {
        let page = Int(getpagesize())
        let bytes = UInt32((surface.allocationSize + page - 1) / page * page)
        surface.lock(options: [], seed: nil)
        let address = UInt64(UInt(bitPattern: surface.baseAddress))
        surface.unlock(options: [], seed: nil)
        let id = IOSurfaceGetID(unsafeBitCast(surface, to: IOSurfaceRef.self))
        let imported = maker.call(boImport, GPUConnection.words(halves(address) + [bytes, 0, id, 0]), outputSize: 16)
        guard imported.0 == KERN_SUCCESS else {
            throw TestFailure("the surface's import was refused: \(GPUConnection.hex(imported.0))")
        }
        let handle = GPUConnection.uint64(imported.1, 0)
        let use = GPUConnection.words(halves(handle) + [surfaceWrite, 0])
        let made = use.withUnsafeBytes { uses -> (kern_return_t, [UInt8]) in
            let at = UInt64(UInt(bitPattern: uses.baseAddress!))
            return maker.call(
                writerPromise, GPUConnection.words(halves(at) + [promiseMake, 1, 0, 1]), outputSize: 8)
        }
        guard made.0 == KERN_SUCCESS else {
            _ = maker.call(boFree, GPUConnection.words(halves(handle)), outputSize: 0)
            throw TestFailure("the writer promise was refused: \(GPUConnection.hex(made.0))")
        }
        return (handle, GPUConnection.uint32(made.1, 0))
    }

    func exercise(_ gpu: GPU, _ context: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let rowBytes = Self.size * 4
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: Self.size, .height: Self.size, .bytesPerElement: 4, .bytesPerRow: rowBytes,
            .allocSize: rowBytes * Self.size, .pixelFormat: 0x4247_5241,
        ]
        guard PathParts.ownRouteRaw(gpu, part: "the test", counters: counters, &metrics) else { return [] }
        guard let surface = IOSurface(properties: properties), let maker = GPUConnection() else {
            throw TestFailure("no IOSurface or TegraGM20B connection")
        }
        surface.lock(options: [], seed: nil)
        for at in stride(from: 0, to: rowBytes * Self.size, by: 4) {
            (surface.baseAddress + at).copyMemory(from: Self.fill, byteCount: 4)
        }
        surface.unlock(options: [], seed: nil)
        let (handle, promise) = try Self.promise(surface, maker)
        defer { _ = maker.call(Self.boFree, GPUConnection.words(Self.halves(handle)), outputSize: 0) }

        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: Self.size, height: Self.size, mipmapped: false)
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        guard let source = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0),
            let copied = gpu.device.makeBuffer(length: rowBytes * Self.size, options: .storageModeShared),
            let commands = gpu.queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder()
        else { throw TestFailure("no surface texture, buffer or command buffer") }
        blit.copy(
            from: source, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
            sourceSize: MTLSize(width: Self.size, height: Self.size, depth: 1), to: copied, destinationOffset: 0,
            destinationBytesPerRow: rowBytes, destinationBytesPerImage: rowBytes * Self.size)
        blit.endEncoding()
        let start = DispatchTime.now()
        let cancel = Cancel()
        commands.commit()
        DispatchQueue.global().asyncAfter(deadline: start + Self.parkMs / 1000) {
            cancel.at = DispatchTime.now()
            let words = GPUConnection.words([0, 0, Self.promiseCancel, 0, promise, 0])
            cancel.result = maker.call(Self.writerPromise, words, outputSize: 8).0
            cancel.done.signal()
        }
        commands.waitUntilCompleted()
        let ended = DispatchTime.now()
        cancel.done.wait()
        let tookMs = Double(ended.uptimeNanoseconds - start.uptimeNanoseconds) / 1e6
        let afterMs = Double(Int64(ended.uptimeNanoseconds) - Int64(cancel.at.uptimeNanoseconds)) / 1e6
        metrics.set("took_ms", String(format: "%.1f", tookMs))
        metrics.set("after_cancel_ms", String(format: "%.1f", afterMs))
        var failures: [String] = []
        if cancel.result != KERN_SUCCESS {
            failures.append("the cancel was refused: \(GPUConnection.hex(cancel.result))")
        }
        if commands.status != .completed { failures.append("the copy ended \(commands.status.rawValue)") }
        if tookMs < Self.parkMs * 0.8 {
            failures.append(String(format: "the copy ended %.1f ms in: it never waited for the promise", tookMs))
        }
        if afterMs > Self.goesOnMs {
            failures.append(
                String(format: "the copy ended %.1f ms after the cancel (at most %.0f)", afterMs, Self.goesOnMs))
        }
        let got = Array(UnsafeRawBufferPointer(start: copied.contents(), count: 4))
        if got != Self.fill { failures.append("the copy read \(got), not the surface's \(Self.fill)") }
        return failures
    }
}

private final class Cancel: @unchecked Sendable {
    var at = DispatchTime.now(), result = KERN_SUCCESS
    let done = DispatchSemaphore(value: 0)
}
