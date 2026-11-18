import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Every writer promise word the CPU releases is seen by the GPU acquire parked on it, at once
/// (drivers/TegraGM20B/SwitchGpuWriterPromise.h): the release is a store to the write-combined fence page, which the
/// parked PBDMA reads from DRAM only once a DSB has completed it. Repeats gpu.path.promise_cancel's park and cancel
/// rounds times: the Metal device's blit of a surface the test's connection promised parks on the promise's word, and
/// 5 ms later the test cancels the promise. A release the GPU never saw would leave its copy parked past it, so each
/// copy must end within lostMs of its cancel (lost: those that did not, judged zero), and the releases counted
/// (promise_cpu_releases) must equal the copies that ended.
/// gpu.path.promise_unkept judges the give-up's own raise the same way (raise_to_end_ms).
struct PromiseReleaseOrderPathTest: GPUPathTest {
    let id = "gpu.path.promise_release_order"
    let name = "Every promise word the CPU releases is seen by the GPU acquire parked on it"
    let counters = ["writer_promises", "promise_waits", "promise_cpu_releases"]
    static let size = 16, rounds = 16, parkMs = 5.0, lostMs = 100.0

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
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: Self.size, height: Self.size, mipmapped: false)
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        guard let source = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0),
            let copied = gpu.device.makeBuffer(length: rowBytes * Self.size, options: .storageModeShared)
        else { throw TestFailure("no surface texture or buffer") }
        let before = try PromiseTotals.read()
        let paths = GPUConnection()
        guard case .counts(let pathsBefore) = GPUPaths.read(paths) else { throw TestFailure("no path counters") }
        var ended = 0, lost = 0, refused = 0, worst = 0.0
        for _ in 0..<Self.rounds {
            let (handle, promise) = try PromiseCancelPathTest.promise(surface, maker)
            defer {
                _ = maker.call(
                    PromiseCancelPathTest.boFree, GPUConnection.words(PromiseCancelPathTest.halves(handle)),
                    outputSize: 0)
            }
            guard let commands = gpu.queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder() else {
                throw TestFailure("no command buffer")
            }
            blit.copy(
                from: source, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                sourceSize: MTLSize(width: Self.size, height: Self.size, depth: 1), to: copied, destinationOffset: 0,
                destinationBytesPerRow: rowBytes, destinationBytesPerImage: rowBytes * Self.size)
            blit.endEncoding()
            commands.commit()
            Thread.sleep(forTimeInterval: Self.parkMs / 1000)
            let cancelled = DispatchTime.now()
            let words = GPUConnection.words([0, 0, PromiseCancelPathTest.promiseCancel, 0, promise, 0])
            if maker.call(PromiseCancelPathTest.writerPromise, words, outputSize: 8).0 != KERN_SUCCESS { refused += 1 }
            commands.waitUntilCompleted()
            let afterMs = PromiseUnkeptPathTest.ms(since: cancelled)
            worst = max(worst, afterMs)
            if commands.status == .completed { ended += 1 }
            if afterMs > Self.lostMs { lost += 1 }
        }
        let after = try PromiseTotals.read()
        guard case .counts(let pathsAfter) = GPUPaths.read(paths) else { throw TestFailure("no path counters") }
        let releases = pathsAfter.taken("promise_cpu_releases", since: pathsBefore)
        metrics.set("rounds", Self.rounds)
        metrics.set("releases", releases)
        metrics.set("ended", ended)
        metrics.set("lost", lost)
        metrics.set("worst_after_cancel_ms", String(format: "%.1f", worst))
        metrics.set("d_abandoned", after.abandoned - before.abandoned)
        var failures: [String] = []
        if refused > 0 { failures.append("\(refused) cancels were refused") }
        if ended != Self.rounds { failures.append("\(Self.rounds - ended) copies did not complete") }
        if releases != ended { failures.append("\(releases) releases against \(ended) copies that ended") }
        if lost > 0 {
            failures.append(
                String(
                    format: "%d releases were not seen at once (the worst copy ended %.0f ms after its cancel)", lost,
                    worst))
        }
        if after.abandoned != before.abandoned {
            failures.append("\(after.abandoned - before.abandoned) promises were given up: a release was lost")
        }
        return failures
    }
}
