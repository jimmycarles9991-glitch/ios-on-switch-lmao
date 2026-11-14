import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A connection's SubmitJob beside its BoImport (drivers/TegraGM20B/TegraGM20BUserClientDispatch.cpp): an import's
/// wiring, zero-fill faults, page walk and clean run before the connection's lock and submit gate, so a command buffer
/// committed while another thread makes textures over fresh IOSurfaces is scheduled without waiting for them. One
/// thread makes textures over new 720x1280 surfaces back to back; this one commits fills of a private buffer meanwhile,
/// timing each from commit to waitUntilScheduled, and the same with no imports first. A submission is beside the
/// imports when its time overlaps one. An import's own wiring is timed on the test's own connection: a fresh surface's
/// BoImport less a second BoImport of it, which adopts the first's wiring (TegraGM20BUserClientBo.cpp sharedWiringFind)
/// and so skips the zero-fill, page walk, nvkm memory and clean (`wire_ms`; both imports' times are logged). Fails when
/// imports add more than that to the submissions' p99. The fills are private because the raw runtime holds its device
/// lock across a surface texture's import (raw_gpu.cpp createSurfaceTexture) and takes it again before a command buffer
/// that wrote a CPU-cached buffer is scheduled (raw_alias.cpp captureCpuCompletion); a private fill's encode and
/// submission never take it, so the two threads meet only in the kernel.
struct ImportBesideSubmitsTest: GPUPathTest {
    let id = "gpu.path.import_beside_submits"
    let name = "Command buffers are scheduled without waiting for another thread's IOSurface imports"
    let counters = ["submit_jobs"]
    static let width = 720, height = 1280, rowBytes = 3072, imports = 16, quiet = 160, minimumBeside = 40
    static let calibrations = 3, fillBytes = 256, shortestWireMs = 0.5, warmFills = 8
    static let boImportGpuReads: UInt32 = 1

    /// One timed span on DispatchTime's clock, in ms
    struct Span {
        let start: Double, end: Double
        var ms: Double { end - start }
        func overlaps(_ other: Span) -> Bool { start < other.end && other.start < end }
    }

    /// The other thread: textures over fresh surfaces one after another, each kept until the timing ends so no BoFree
    /// (still a mutation behind the submit gate) lands among the submissions.
    final class Importer: @unchecked Sendable {
        private let lock = NSLock()
        private var running = true
        private(set) var spans: [Span] = []
        private(set) var made: [(IOSurface, MTLTexture)] = []
        private(set) var failure: String?
        private let finished = DispatchSemaphore(value: 0)

        var isRunning: Bool {
            lock.lock()
            defer { lock.unlock() }
            return running
        }

        func start(_ device: MTLDevice) {
            Thread { [self] in
                for _ in 0..<ImportBesideSubmitsTest.imports {
                    guard let surface = ImportBesideSubmitsTest.surface() else {
                        failure = "an IOSurface was not made"
                        break
                    }
                    let start = ImportBesideSubmitsTest.nowMs()
                    guard
                        let texture = device.makeTexture(
                            descriptor: ImportBesideSubmitsTest.sampled, iosurface: surface, plane: 0)
                    else {
                        failure = "makeTexture(iosurface:) returned nil"
                        break
                    }
                    spans.append(Span(start: start, end: ImportBesideSubmitsTest.nowMs()))
                    made.append((surface, texture))
                }
                lock.lock()
                running = false
                lock.unlock()
                finished.signal()
            }.start()
        }

        func wait() { finished.wait() }
    }

    static var sampled: MTLTextureDescriptor {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        return descriptor
    }

    static func surface() -> IOSurface? {
        IOSurface(properties: [
            .width: width, .height: height, .bytesPerElement: 4, .bytesPerRow: rowBytes, .allocSize: rowBytes * height,
            .pixelFormat: 0x4247_5241,
        ])
    }

    static func nowMs() -> Double { Double(DispatchTime.now().uptimeNanoseconds) / 1e6 }

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        guard let connection = GPUConnection() else { throw TestFailure("no TegraGM20B connection") }
        guard let target = gpu.device.makeBuffer(length: Self.fillBytes, options: .storageModePrivate) else {
            throw TestFailure("no private buffer")
        }
        let (wireMs, firstMs, secondMs) = try Self.wiring(connection)
        for _ in 0..<Self.warmFills { _ = try Self.submit(gpu, target) }
        let quiet = try (0..<Self.quiet).map { _ in try Self.submit(gpu, target) }
        let importer = Importer()
        importer.start(gpu.device)
        var during: [Span] = []
        while importer.isRunning { during.append(try Self.submit(gpu, target)) }
        importer.wait()
        let beside = during.filter { submission in importer.spans.contains { $0.overlaps(submission) } }
        let quietP99 = Self.p99(quiet), besideP99 = Self.p99(beside)
        let added = besideP99 - quietP99
        metrics.set("wire_ms", wireMs, digits: 2)
        metrics.set("boimport_ms", firstMs, digits: 2)
        metrics.set("boimport_again_ms", secondMs, digits: 2)
        metrics.set("import_ms", Self.median(importer.spans.map(\.ms)), digits: 2)
        metrics.set("imports", importer.made.count)
        metrics.set("beside", beside.count)
        metrics.set("quiet_p99_ms", quietP99, digits: 2)
        metrics.set("quiet_worst_ms", quiet.map(\.ms).max() ?? 0, digits: 2)
        metrics.set("beside_p99_ms", besideP99, digits: 2)
        metrics.set("beside_worst_ms", beside.map(\.ms).max() ?? 0, digits: 2)
        metrics.set("added_p99_ms", added, digits: 2)
        withExtendedLifetime(importer.made) {}
        var failures: [String] = []
        if let failure = importer.failure { failures.append(failure) }
        if wireMs < Self.shortestWireMs {
            failures.append(String(format: "an import's wiring took %.2f ms: too short to tell", wireMs))
        }
        if beside.count < Self.minimumBeside {
            failures.append("\(beside.count) submissions beside the imports (at least \(Self.minimumBeside))")
        }
        if added > wireMs {
            failures.append(
                String(
                    format: "imports added %.2f ms to the p99 submission, more than an import's %.2f ms wiring", added,
                    wireMs))
        }
        return failures
    }

    /// One fill of the private buffer: commit to scheduled, then waited for to completion outside the span.
    static func submit(_ gpu: GPU, _ target: MTLBuffer) throws -> Span {
        guard let commands = gpu.queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder() else {
            throw TestFailure("no command buffer")
        }
        blit.fill(buffer: target, range: 0..<fillBytes, value: 0x5a)
        blit.endEncoding()
        let start = nowMs()
        commands.commit()
        commands.waitUntilScheduled()
        let span = Span(start: start, end: nowMs())
        try gpu.finish([commands])
        return span
    }

    /// On the test's own connection, the median over fresh surfaces: the first BoImport less a second BoImport of the
    /// same pages, and each import's own time, in ms.
    static func wiring(_ connection: GPUConnection) throws -> (Double, Double, Double) {
        var wires: [Double] = [], firsts: [Double] = [], seconds: [Double] = []
        for _ in 0..<calibrations {
            guard let surface = surface() else { throw TestFailure("an IOSurface was not made") }
            let first = try boImport(connection, surface)
            let second = try boImport(connection, surface)
            for (handle, _) in [first, second] {
                _ = connection.call(
                    GPUConnection.boFreeMethod, GPUConnection.words(halves(handle)), outputSize: 0)
            }
            firsts.append(first.ms)
            seconds.append(second.ms)
            wires.append(first.ms - second.ms)
        }
        return (median(wires), median(firsts), median(seconds))
    }

    static func boImport(_ connection: GPUConnection, _ surface: IOSurface) throws -> (handle: UInt64, ms: Double) {
        let page = Int(getpagesize())
        let bytes = UInt64((surface.allocationSize + page - 1) / page * page)
        let address = UInt64(UInt(bitPattern: surface.baseAddress))
        let id = IOSurfaceGetID(surface)
        let start = nowMs()
        let (kr, reply) = connection.call(
            GPUConnection.boImportMethod,
            GPUConnection.words(halves(address) + halves(bytes) + [id, boImportGpuReads]), outputSize: 16)
        let ms = nowMs() - start
        guard kr == KERN_SUCCESS else { throw TestFailure("BoImport of a fresh surface: \(GPUConnection.hex(kr))") }
        return (GPUConnection.uint64(reply, 0), ms)
    }

    static func halves(_ value: UInt64) -> [UInt32] { [UInt32(truncatingIfNeeded: value), UInt32(value >> 32)] }

    static func p99(_ spans: [Span]) -> Double {
        let sorted = spans.map(\.ms).sorted()
        guard !sorted.isEmpty else { return 0 }
        return sorted[min(sorted.count - 1, Int((Double(sorted.count) * 0.99).rounded(.up)) - 1)]
    }

    static func median(_ values: [Double]) -> Double {
        let sorted = values.sorted()
        return sorted.isEmpty ? 0 : sorted[sorted.count / 2]
    }
}
