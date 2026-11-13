import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal
import UIKit

/// Writer promises given up on since boot, from kern.switch_gpu_promises (debug kernels): each is reported once in a
/// "writer promise ... was not kept" line, and lastWaitedMs is how long the last one's reader had waited. direct are
/// the direct presents of promised frames: watched on their promise, handed to the job that kept it, given up.
struct PromiseTotals {
    let abandoned: Int, lastWaitedMs: Int
    let directPromised: Int, directKept: Int, directGivenUp: Int

    static func read() throws -> PromiseTotals {
        switch KernelSysctl.string("kern.switch_gpu_promises") {
        case .value(let line):
            var fields: [String: Int] = [:]
            for field in line.split(separator: " ") {
                let pair = field.split(separator: "=", maxSplits: 1).map(String.init)
                if pair.count == 2 { fields[pair[0]] = Int(pair[1]) }
            }
            return PromiseTotals(
                abandoned: fields["abandoned"] ?? -1, lastWaitedMs: fields["last_waited_ms"] ?? -1,
                directPromised: fields["direct_promised"] ?? -1, directKept: fields["direct_kept"] ?? -1,
                directGivenUp: fields["direct_given_up"] ?? -1)
        case .failed(let error):
            throw TestFailure(KernelSysctl.unreadable("kern.switch_gpu_promises", error))
        }
    }
}

/// Lets a reader go if nothing gave its promise up by `afterSeconds`, so a failing run ends, and says whether it had
/// to: a bound, never how a passing run ends.
final class PromiseSafetyCancel: @unchecked Sendable {
    private(set) var fired = false
    private var done = false
    private let lock = NSLock()

    init(_ maker: GPUConnection, promise: UInt32, afterSeconds: Double) {
        DispatchQueue.global().asyncAfter(deadline: .now() + afterSeconds) { [self] in
            lock.lock()
            defer { lock.unlock() }
            guard !done else { return }
            fired = true
            let words = GPUConnection.words([0, 0, PromiseCancelPathTest.promiseCancel, 0, promise, 0])
            _ = maker.call(PromiseCancelPathTest.writerPromise, words, outputSize: 8)
        }
    }

    func finish() {
        lock.lock()
        done = true
        lock.unlock()
    }
}

/// A writer promise nobody keeps (drivers/TegraGM20B/SwitchGpuWriterPromise.h): the test's connection promises an
/// IOSurface's writes and never submits the job. Its readers must not wait for ever, nor see the surface without the
/// frame it held: the promise is given up once, a second after its first reader parked (SwitchGpuPromiseGiveUp.h),
/// its writer reported in one line, and each reader goes on with the frame the surface held before.
/// - composite: a layer showing the surface, rendered by the render server, whose job acquires the promise and parks
///   at its submit, so the display's present waits behind that job: composite_ms about 1000. Judged on its own
///   evidence: a composite that never parked (nothing given up, no safety cancel) is SKIP with that reason, its promise
///   cancelled; one that parked fails only on its give-up count, the frame it shows or its time.
/// - copy: the Metal device's blit of the surface (this process's own job, run only when its command buffers take the
///   raw path). It parks at its submit: copy_ms about 1000. After the raise the parked channel must see the
///   CPU-written word at once (raise_to_end_ms under oneStrikeMs), the give-up's DSB. Alongside it a CPU lock of the
///   surface sleeps behind the same promise and parks on it too; the give-up wakes it, with no cancel or close, and it
///   reads the frame before (cpu_reader_woken_ms about copy_ms).
/// Each gives up exactly one promise (d_abandoned 1).
struct PromiseUnkeptPathTest: GPUPathTest {
    let id = "gpu.path.promise_unkept"
    let name = "A writer promise nobody keeps is given up once and its readers go on with the frame before it"
    let counters = ["writer_promises", "promise_waits"]
    static let size = 64
    static let fill: [UInt8] = [0, 0, 255, 255]  // BGRA red: the frame before the promised one
    static let compositeLimitMs = 1100.0, copyLimitMs = 1100.0, oneStrikeMs = 500.0, safetySeconds = 8.0

    func exercise(_ gpu: GPU, _ context: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let rowBytes = Self.size * 4
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: Self.size, .height: Self.size, .bytesPerElement: 4, .bytesPerRow: rowBytes,
            .allocSize: rowBytes * Self.size, .pixelFormat: 0x4247_5241,
        ]
        guard let surface = IOSurface(properties: properties), let maker = GPUConnection() else {
            throw TestFailure("no IOSurface or TegraGM20B connection")
        }
        surface.lock(options: [], seed: nil)
        for at in stride(from: 0, to: rowBytes * Self.size, by: 4) {
            (surface.baseAddress + at).copyMemory(from: Self.fill, byteCount: 4)
        }
        surface.unlock(options: [], seed: nil)
        var failures: [String] = []

        let start = try PromiseTotals.read()
        let (handle, compositePromise) = try PromiseCancelPathTest.promise(surface, maker)
        defer {
            _ = maker.call(
                PromiseCancelPathTest.boFree, GPUConnection.words(PromiseCancelPathTest.halves(handle)), outputSize: 0)
        }
        let shown = await context.onStage { stage -> UIView in
            let view = UIView(frame: CGRect(x: 0, y: 0, width: 96, height: 96))
            view.layer.contents = surface
            view.layer.contentsGravity = .resize
            stage.addSubview(view)
            return view
        }
        let compositeSafety = PromiseSafetyCancel(maker, promise: compositePromise, afterSeconds: Self.safetySeconds)
        let rendering = DispatchTime.now()
        let render = await MainActor.run { Snapshots.composited(shown) }
        let compositeMs = Self.ms(since: rendering)
        compositeSafety.finish()
        await context.clearStage()
        let afterComposite = try PromiseTotals.read()
        let centre = render.pixel(render.width / 2, render.height / 2)
        let compositeGivenUp = afterComposite.abandoned - start.abandoned
        metrics.set("composite_ms", String(format: "%.1f", compositeMs))
        metrics.set("composite_waited_ms", afterComposite.lastWaitedMs)
        metrics.set("composite_d_abandoned", compositeGivenUp)
        metrics.set(
            "centre", centre.prefix(3).map { String(Int(($0 * render.levels).rounded())) }.joined(separator: ","))
        if compositeGivenUp == 0 && !compositeSafety.fired {
            // its promise stays made otherwise, and the copy's job below would acquire it too
            let words = GPUConnection.words([0, 0, PromiseCancelPathTest.promiseCancel, 0, compositePromise, 0])
            _ = maker.call(PromiseCancelPathTest.writerPromise, words, outputSize: 8)
            PathParts.current?.skip(
                String(format: "composite: the render server never parked on the promise (%.0f ms)", compositeMs),
                counters: [])
        } else {
            if compositeSafety.fired { failures.append("the composite went on only at the safety cancel") }
            if compositeGivenUp != 1 {
                failures.append("the composite's promise was given up \(compositeGivenUp) times")
            }
            if compositeMs > Self.compositeLimitMs {
                failures.append(
                    String(format: "the composite waited %.0f ms (at most %.0f)", compositeMs, Self.compositeLimitMs))
            }
            if centre[0] < 0.8 || centre[1] > 0.2 {
                failures.append("the composite shows \(centre.prefix(3)), not the frame before (red)")
            }
        }

        guard PathParts.ownRouteRaw(gpu, part: "copy and CPU lock", counters: ["promise_waits"], &metrics) else {
            return failures
        }
        let (copyHandle, copyPromise) = try PromiseCancelPathTest.promise(surface, maker)
        defer {
            _ = maker.call(
                PromiseCancelPathTest.boFree, GPUConnection.words(PromiseCancelPathTest.halves(copyHandle)),
                outputSize: 0)
        }
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
        let copySafety = PromiseSafetyCancel(maker, promise: copyPromise, afterSeconds: Self.safetySeconds)
        let copying = DispatchTime.now()
        let cpuReader = CpuReader(surface)
        commands.commit()
        commands.waitUntilCompleted()
        let copyMs = Self.ms(since: copying)
        let cpuRead = cpuReader.wait()
        copySafety.finish()
        metrics.set("cpu_reader_woken_ms", String(format: "%.1f", cpuRead.ms))
        if cpuRead.ms > Self.copyLimitMs {
            failures.append(String(format: "the CPU lock waited %.0f ms (at most %.0f)", cpuRead.ms, Self.copyLimitMs))
        }
        if cpuRead.pixel != Self.fill {
            failures.append("the CPU lock read \(cpuRead.pixel), not the frame before \(Self.fill)")
        }
        let afterCopy = try PromiseTotals.read()
        let raiseToEndMs = copyMs - Double(afterCopy.lastWaitedMs)
        metrics.set("copy_ms", String(format: "%.1f", copyMs))
        metrics.set("copy_waited_ms", afterCopy.lastWaitedMs)
        metrics.set("raise_to_end_ms", String(format: "%.1f", raiseToEndMs))
        metrics.set("copy_d_abandoned", afterCopy.abandoned - afterComposite.abandoned)
        let safetyMs = Self.safetySeconds * 1000
        if copySafety.fired && copyMs >= safetyMs { failures.append("the copy went on only at the safety cancel") }
        if copySafety.fired && cpuRead.ms >= safetyMs {
            failures.append("the CPU lock went on only at the safety cancel: the give-up did not wake it")
        }
        if afterCopy.abandoned - afterComposite.abandoned != 1 {
            failures.append("the copy's promise was given up \(afterCopy.abandoned - afterComposite.abandoned) times")
        }
        if commands.status != .completed { failures.append("the copy ended \(commands.status.rawValue)") }
        if copyMs > Self.copyLimitMs {
            failures.append(String(format: "the copy waited %.0f ms (at most %.0f)", copyMs, Self.copyLimitMs))
        }
        if raiseToEndMs > Self.oneStrikeMs {
            failures.append(
                String(format: "the copy ended %.0f ms after its give-up: the raised word was not seen", raiseToEndMs))
        }
        let got = Array(UnsafeRawBufferPointer(start: copied.contents(), count: 4))
        if got != Self.fill { failures.append("the copy read \(got), not the frame before \(Self.fill)") }
        return failures
    }

    static func ms(since start: DispatchTime) -> Double {
        Double(DispatchTime.now().uptimeNanoseconds - start.uptimeNanoseconds) / 1e6
    }
}

/// A read-only CPU lock of the surface on its own thread: how long it slept and the first pixel it read.
final class CpuReader: @unchecked Sendable {
    private let done = DispatchSemaphore(value: 0)
    private var result: (ms: Double, pixel: [UInt8]) = (0, [])

    init(_ surface: IOSurface) {
        let start = DispatchTime.now()
        DispatchQueue.global().async { [self] in
            surface.lock(options: .readOnly, seed: nil)
            let ms = PromiseUnkeptPathTest.ms(since: start)
            let pixel = Array(UnsafeRawBufferPointer(start: surface.baseAddress, count: 4))
            surface.unlock(options: .readOnly, seed: nil)
            result = (ms, pixel)
            done.signal()
        }
    }

    func wait() -> (ms: Double, pixel: [UInt8]) {
        done.wait()
        return result
    }
}
