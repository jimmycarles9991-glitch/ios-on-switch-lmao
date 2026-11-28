import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import IOSurface
import Metal
import UIKit

/// Writer promises given up on since boot, from kern.switch_gpu_promises (debug kernels): each is reported once in a
/// "writer promise ... was not kept" line, and lastWaitedMs is how long the last one's reader had waited. direct are
/// the direct presents of promised frames: watched on their promise, handed to the job that kept it, given up.
public struct PromiseTotals {
    public let abandoned: Int, lastWaitedMs: Int
    public let directPromised: Int, directKept: Int, directGivenUp: Int

    public static func read() throws -> PromiseTotals {
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
                directGivenUp: fields["direct_given_up"] ?? -1
            )
        case .failed(let error):
            throw TestFailure(KernelSysctl.unreadable("kern.switch_gpu_promises", error, flag: "--//bazel:stats_gpu"))
        }
    }
}

/// The maker's cancel of its promise (TegraGM20BJobABI.h method 34), `afterSeconds` from its making or at once through
/// `cancel()`, whichever comes first, made once; `at` is when it was made (nil before).
public final class PromiseMakerCancel: @unchecked Sendable {
    private let lock = NSLock()
    private let maker: GPUConnection, promise: UInt32
    private var made: DispatchTime?
    private var answer = KERN_SUCCESS

    public init(_ maker: GPUConnection, promise: UInt32, afterSeconds: Double) {
        self.maker = maker
        self.promise = promise
        DispatchQueue.global().asyncAfter(deadline: .now() + afterSeconds) { [self] in cancel() }
    }

    public func cancel() {
        lock.lock()
        defer { lock.unlock() }
        guard made == nil else { return }
        made = DispatchTime.now()
        let words = GPUConnection.words([0, 0, PromiseCancelPathTest.promiseCancel, 0, promise, 0])
        answer = maker.call(PromiseCancelPathTest.writerPromise, words, outputSize: 8).0
    }

    public var at: DispatchTime? {
        lock.lock()
        defer { lock.unlock() }
        return made
    }

    public var result: kern_return_t {
        lock.lock()
        defer { lock.unlock() }
        return answer
    }
}

/// A writer promise nobody keeps or arms (drivers/TegraGM20B/SwitchGpuWriterPromise.h): the test's connection promises
/// an IOSurface's writes and then does nothing with it, as a holder that stopped right after its commit's make, before
/// its submit worker armed a bound (iOS suspended it, a debugger stopped it). Its readers do not wait for it to run
/// again: the kernel gives every unkept promise up at TegraGM20BPromiseBoundNs, Metal's 5 s, from its first reader's
/// park when no holder armed its own (SwitchGpuPromiseGiveUp.h). All readers wait on the one promise at once, so the
/// test takes one bound:
/// - copy: the Metal device's blit of the surface (this process's own job, run only when its command buffers take the
///   raw path), parked at its submit, and beside it a CPU lock of the surface that sleeps behind the same promise;
/// - composite: a layer showing the surface, rendered by the render server, whose job acquires the promise and parks
///   at its submit, so the display's present waits behind that job. A composite that ended before the bound never
///   parked: SKIP with that reason.
/// Judged: the promise given up once (d_abandoned 1, one "was not kept" line), the kernel's own wait from the first
/// park to the give-up (waited_ms) at the bound and at most boundLateMs past it (the lateness of the give-up's one-shot
/// thread call); each reader gone on no sooner than the bound after the test began and at most readerLateMs past it
/// (its park after the start, then PromiseCancelPathTest.goesOnMs for a parked reader to see the raised word); no
/// reader left for the safety cancel; the frame before shown and read; the copy completed; and the maker's cancel of
/// the promise the kernel gave up still accepted.
public struct PromiseUnkeptPathTest: GPUPathTest {
    public let id = "gpu.path.promise_unkept"
    public let name = "A promise nobody keeps or arms is given up at the 5 s bound, its readers seeing the frame before"
    public let counters = ["writer_promises", "promise_waits"]
    public static let size = 64
    public static let fill: [UInt8] = [0, 0, 255, 255] // BGRA red: the frame before the promised one
    public static let boundMs = 5000.0, boundLateMs = 50.0, safetySeconds = 8.0
    public static let readerLateMs = boundLateMs + PromiseCancelPathTest.goesOnMs

    public init() {}

    public func exercise(_ gpu: GPU, _ context: TestContext, _ metrics: inout Metrics) async throws -> [String] {
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

        let raw = PathParts.ownRouteRaw(gpu, part: "copy and CPU lock", counters: ["promise_waits"], &metrics)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: Self.size, height: Self.size, mipmapped: false
        )
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        guard let source = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0),
              let copied = gpu.device.makeBuffer(length: rowBytes * Self.size, options: .storageModeShared),
              let commands = gpu.queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder()
        else { throw TestFailure("no surface texture, buffer or command buffer") }
        blit.copy(
            from: source, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
            sourceSize: MTLSize(width: Self.size, height: Self.size, depth: 1), to: copied, destinationOffset: 0,
            destinationBytesPerRow: rowBytes, destinationBytesPerImage: rowBytes * Self.size
        )
        blit.endEncoding()

        let start = try PromiseTotals.read()
        let (handle, promise) = try PromiseCancelPathTest.promise(surface, maker)
        defer {
            _ = maker.call(
                PromiseCancelPathTest.boFree, GPUConnection.words(PromiseCancelPathTest.halves(handle)), outputSize: 0
            )
        }
        let shown = await context.onStage { stage -> UIView in
            let view = UIView(frame: CGRect(x: 0, y: 0, width: 96, height: 96))
            view.layer.contents = surface
            view.layer.contentsGravity = .resize
            stage.addSubview(view)
            return view
        }
        let safety = PromiseMakerCancel(maker, promise: promise, afterSeconds: Self.safetySeconds)
        let begun = DispatchTime.now()
        var cpuReader: CpuReader?
        if raw {
            commands.commit()
            cpuReader = CpuReader(surface)
        }
        let render = await MainActor.run { Snapshots.composited(shown) }
        let compositeEnded = DispatchTime.now()
        await context.clearStage()
        var copyEnded = compositeEnded
        if raw {
            commands.waitUntilCompleted()
            copyEnded = DispatchTime.now()
        }
        let cpuRead = cpuReader?.wait()
        let safetyFired = safety.at != nil
        safety.cancel()
        let after = try PromiseTotals.read()

        let givenUp = after.abandoned - start.abandoned
        let compositeMs = Self.ms(begun, compositeEnded)
        let centre = render.pixel(render.width / 2, render.height / 2)
        let compositeParked = compositeMs >= Self.boundMs
        metrics.set("d_abandoned", givenUp)
        metrics.set("waited_ms", after.lastWaitedMs)
        metrics.set("composite_ms", String(format: "%.1f", compositeMs))
        metrics.set(
            "centre", centre.prefix(3).map { String(Int(($0 * render.levels).rounded())) }.joined(separator: ",")
        )
        if safetyFired { failures.append("a reader went on only at the safety cancel, \(Self.safetySeconds) s on") }
        if safety.result != KERN_SUCCESS {
            failures.append("the maker's cancel after the give-up was refused: \(GPUConnection.hex(safety.result))")
        }
        if !compositeParked {
            PathParts.current?.skip(
                String(format: "composite: the render server never parked on the promise (%.0f ms)", compositeMs),
                counters: []
            )
        }
        guard compositeParked || raw else {
            if givenUp != 0 { failures.append("a promise nobody waited on was given up \(givenUp) times") }
            return failures
        }
        if givenUp != 1 { failures.append("the unkept promise was given up \(givenUp) times, not once") }
        if Double(after.lastWaitedMs) < Self.boundMs || Double(after.lastWaitedMs) > Self.boundMs + Self.boundLateMs {
            failures.append(
                String(
                    format: "given up %ld ms after its first reader parked (the bound is %.0f, at most %.0f late)",
                    after.lastWaitedMs, Self.boundMs, Self.boundLateMs
                )
            )
        }
        if compositeParked {
            failures += Self.late("composite", compositeMs)
            if centre[0] < 0.8 || centre[1] > 0.2 {
                failures.append("the composite shows \(centre.prefix(3)), not the frame before (red)")
            }
        }
        if let cpuRead = cpuRead {
            let copyMs = Self.ms(begun, copyEnded), cpuMs = Self.ms(begun, cpuRead.woken)
            metrics.set("copy_ms", String(format: "%.1f", copyMs))
            metrics.set("cpu_reader_woken_ms", String(format: "%.1f", cpuMs))
            failures += Self.late("copy", copyMs) + Self.late("CPU lock", cpuMs)
            let got = Array(UnsafeRawBufferPointer(start: copied.contents(), count: 4))
            failures += Self.frameBefore(cpuLock: cpuRead.pixel, copy: got, copyStatus: commands.status)
        }
        return failures
    }

    /// A reader that went on `ms` after the test began: never before the bound, at most readerLateMs past it
    public static func late(_ reader: String, _ ms: Double) -> [String] {
        if ms < boundMs { return [String(format: "the %@ went on %.0f ms in, before the bound", reader, ms)] }
        guard ms > boundMs + readerLateMs else { return [] }
        return [
            String(
                format: "the %@ went on %.0f ms in (the bound is %.0f, at most %.0f late)", reader, ms, boundMs,
                readerLateMs
            ),
        ]
    }

    /// What the copy and the CPU lock read once the promise was given up: the frame before, the copy completed
    public static func frameBefore(cpuLock: [UInt8], copy: [UInt8], copyStatus: MTLCommandBufferStatus) -> [String] {
        var failures: [String] = []
        if cpuLock != fill { failures.append("the CPU lock read \(cpuLock), not the frame before \(fill)") }
        if copyStatus != .completed { failures.append("the copy ended \(copyStatus.rawValue)") }
        if copy != fill { failures.append("the copy read \(copy), not the frame before \(fill)") }
        return failures
    }

    public static func ms(_ from: DispatchTime, _ to: DispatchTime) -> Double {
        Double(Int64(to.uptimeNanoseconds) - Int64(from.uptimeNanoseconds)) / 1e6
    }

    public static func ms(since start: DispatchTime) -> Double {
        Double(DispatchTime.now().uptimeNanoseconds - start.uptimeNanoseconds) / 1e6
    }
}

/// A read-only CPU lock of the surface on its own thread: when it woke and the first pixel it read.
public final class CpuReader: @unchecked Sendable {
    private let done = DispatchSemaphore(value: 0)
    private var result: (woken: DispatchTime, pixel: [UInt8]) = (DispatchTime.now(), [])

    public init(_ surface: IOSurface) {
        DispatchQueue.global().async { [self] in
            surface.lock(options: .readOnly, seed: nil)
            let woken = DispatchTime.now()
            let pixel = Array(UnsafeRawBufferPointer(start: surface.baseAddress, count: 4))
            surface.unlock(options: .readOnly, seed: nil)
            result = (woken, pixel)
            done.signal()
        }
    }

    public func wait() -> (woken: DispatchTime, pixel: [UInt8]) {
        done.wait()
        return result
    }
}
