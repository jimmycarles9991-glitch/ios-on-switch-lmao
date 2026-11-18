import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Two threads CPU-lock two IOSurfaces the GPU is still writing, at the same time: each lock waits for its own
/// surface's writer and no longer, as on Apple's GPUs, so neither sleeps through the other surface's GPU work. One
/// queue runs both writers in order, B's long render then A's, each about `targetMs` of full-surface draws (sized by a
/// timed render of a third surface made the same way). One thread locks A; 2 ms later another locks B. B's writer ends
/// where A's starts, so B's lock must return before A's by at least half of A's GPU time. Reported: each lock's wait,
/// how long both waited at once (`overlap_ms`) and the pixel each lock read (the render's colour, or the lock did not
/// wait for its writer). A lock waits on every live channel's work (gpu_sync.c tegra_gm20b_sync_for_cpu) only for a
/// surface whose writer slots overflowed, five channels with unfinished writes of it, which one process cannot make;
/// the debug line "tegra-gm20b: unlocked waits: N CPU syncs beside another" counts those syncs on a device. The
/// surfaces: block-linear, as the GPU's first write decides a fresh one (TegraGM20BTiled.cpp), or (`_pitch`) pitch:
/// written once under a CPU lock, so its seed has moved, then first imported by a texture that only samples it, which
/// decides pitch for good.
struct SurfaceLocksSideBySideTest: GPUPathTest {
    let id: String
    let name: String
    let pitch: Bool
    let counters: [String] = []
    static let side = 1024, warmDraws = 64, targetMs = 40.0, staggerMs = 2.0
    static let colourA: [Float] = [0, 0, 1, 1], colourB: [Float] = [0, 1, 0, 1]

    static let blockLinear = SurfaceLocksSideBySideTest(
        id: "gpu.path.cpu_locks_side_by_side",
        name: "Two CPU locks of surfaces the GPU is writing each wait for their own writer only", pitch: false)
    static let pitchLinear = SurfaceLocksSideBySideTest(
        id: "gpu.path.cpu_locks_side_by_side_pitch",
        name: "Two CPU locks of pitch surfaces the GPU is writing each wait for their own writer only", pitch: true)

    struct Target {
        let surface: IOSurface
        let texture: MTLTexture
        /// The sampling texture that made the surface pitch, held while the test runs
        let sampler: MTLTexture?
    }

    /// One thread's read-only CPU lock of a surface: when it asked, when the lock returned, and the centre texel it read.
    final class Locker: @unchecked Sendable {
        let surface: IOSurface
        private(set) var askedMs = 0.0, returnedMs = 0.0
        private(set) var result: kern_return_t = KERN_SUCCESS
        private(set) var texel: [UInt8] = []
        private let done = DispatchSemaphore(value: 0)

        init(_ surface: IOSurface) { self.surface = surface }

        func start() {
            Thread { [self] in
                askedMs = SurfaceLocksSideBySideTest.nowMs()
                result = surface.lock(options: .readOnly, seed: nil)
                returnedMs = SurfaceLocksSideBySideTest.nowMs()
                if result == KERN_SUCCESS {
                    let centre = surface.baseAddress + (surface.height / 2) * surface.bytesPerRow + surface.width * 2
                    texel = Array(UnsafeRawBufferPointer(start: centre, count: 4))
                    surface.unlock(options: .readOnly, seed: nil)
                }
                done.signal()
            }.start()
        }

        func wait() { done.wait() }
        var waitedMs: Double { returnedMs - askedMs }
    }

    /// When each writer's completed handler ran
    final class Ends: @unchecked Sendable {
        private let lock = NSLock()
        private var ends: [String: Double] = [:]
        private let noted = DispatchGroup()

        func handler(_ name: String) -> @Sendable (MTLCommandBuffer) -> Void {
            noted.enter()
            return { [self] _ in
                let now = SurfaceLocksSideBySideTest.nowMs()
                lock.lock()
                ends[name] = now
                lock.unlock()
                noted.leave()
            }
        }

        /// Every handler made ran within `seconds`
        func wait(_ seconds: Double) -> Bool { noted.wait(timeout: .now() + seconds) == .success }

        subscript(_ name: String) -> Double {
            lock.lock()
            defer { lock.unlock() }
            return ends[name] ?? .infinity
        }
    }

    static func nowMs() -> Double { Double(DispatchTime.now().uptimeNanoseconds) / 1e6 }

    /// BGRA bytes of an RGBA colour
    static func bgra(_ rgba: [Float]) -> [UInt8] { [2, 1, 0, 3].map { UInt8((rgba[$0] * 255).rounded()) } }

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let probe = try Self.target(gpu, pitch: pitch)
        var warm = try await gpu.run { Self.encode($0, probe.texture, pipeline, Self.colourB, draws: Self.warmDraws) }
        // the second, without the first's one-time costs
        warm = try await gpu.run { Self.encode($0, probe.texture, pipeline, Self.colourA, draws: Self.warmDraws) }
        let perDrawMs = (warm.gpuTimed ? warm.gpuMilliseconds : warm.wallMilliseconds) / Double(Self.warmDraws)
        let draws = min(8000, max(Self.warmDraws, Int(Self.targetMs / max(perDrawMs, 0.001))))
        let jobMs = perDrawMs * Double(draws)
        metrics.set("draws", draws)
        metrics.set("job_ms", jobMs, digits: 1)

        let a = try Self.target(gpu, pitch: pitch), b = try Self.target(gpu, pitch: pitch)
        let ends = Ends()
        let writeB = try gpu.commit { commands in
            Self.encode(commands, b.texture, pipeline, Self.colourB, draws: draws)
            commands.addCompletedHandler(ends.handler("b"))
        }
        let writeA = try gpu.commit { commands in
            Self.encode(commands, a.texture, pipeline, Self.colourA, draws: draws)
            commands.addCompletedHandler(ends.handler("a"))
        }
        let (lockA, lockB) = Self.lockBoth(a.surface, b.surface, after: [writeB, writeA])
        try gpu.finish([writeB, writeA])

        let ahead = lockA.returnedMs - lockB.returnedMs
        let overlap = min(lockA.returnedMs, lockB.returnedMs) - max(lockA.askedMs, lockB.askedMs)
        metrics.set("lock_a_ms", lockA.waitedMs, digits: 1)
        metrics.set("lock_b_ms", lockB.waitedMs, digits: 1)
        metrics.set("overlap_ms", overlap, digits: 1)
        metrics.set("b_ahead_ms", ahead, digits: 1)
        metrics.set("a_gpu_ms", max(0, writeA.gpuEndTime - writeA.gpuStartTime) * 1000, digits: 1)
        var failures: [String] = []
        for (label, locker, colour) in [("A", lockA, Self.colourA), ("B", lockB, Self.colourB)] {
            if locker.result != KERN_SUCCESS {
                failures.append("the lock of \(label) returned \(GPUConnection.hex(locker.result))")
            } else if locker.texel != Self.bgra(colour) {
                failures.append("the lock of \(label) read \(locker.texel), not its render's \(Self.bgra(colour))")
            }
        }
        if !ends.wait(5) {
            failures.append("the writers' completed handlers never ran")
        } else if lockB.askedMs >= ends["b"] || lockA.askedMs >= ends["a"] {
            failures.append("a lock was asked after its writer ended: nothing measured")
        }
        if ahead < jobMs / 2 {
            failures.append(
                String(format: "B's lock was %.1f ms ahead of A's, under half of A's %.1f ms render", ahead, jobMs))
        }
        if let connection = GPUConnection() {
            for (label, target) in [("a", a), ("b", b)] {
                let (kr, layout) = connection.surfaceLayout(target.surface)
                let answer = kr == KERN_SUCCESS ? String(layout) : GPUConnection.hex(kr)
                metrics.set("\(label)_layout", answer)
                if answer != (pitch ? "0" : "1") {
                    failures.append(
                        "surface \(label.uppercased()) is laid out \(answer), not \(pitch ? "pitch" : "block-linear")")
                }
            }
        }
        return failures
    }

    /// Once both writers reached the kernel: A's lock on one thread, B's on another `staggerMs` later
    static func lockBoth(_ a: IOSurface, _ b: IOSurface, after writers: [MTLCommandBuffer]) -> (Locker, Locker) {
        for writer in writers { writer.waitUntilScheduled() }
        let lockA = Locker(a), lockB = Locker(b)
        lockA.start()
        usleep(useconds_t(staggerMs * 1000))
        lockB.start()
        lockA.wait()
        lockB.wait()
        return (lockA, lockB)
    }

    /// A clear, then `draws` full-surface quads of `colour`
    static func encode(
        _ commands: MTLCommandBuffer, _ target: MTLTexture, _ pipeline: MTLRenderPipelineState, _ colour: [Float],
        draws: Int
    ) {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.5, green: 0.5, blue: 0.5, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(pipeline)
        var corners = GPU.pixelQuad(0, 0, side, side, width: side, height: side)
        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        var rgba = colour
        encoder.setFragmentBytes(&rgba, length: MemoryLayout<Float>.stride * 4, index: 0)
        for _ in 0..<draws { encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4) }
        encoder.endEncoding()
    }

    static func target(_ gpu: GPU, pitch: Bool) throws -> Target {
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: side, .height: side, .bytesPerElement: 4, .bytesPerRow: side * 4, .allocSize: side * side * 4,
            .pixelFormat: 0x4247_5241,
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface not made") }
        if pitch {
            surface.lock(options: [], seed: nil)
            surface.unlock(options: [], seed: nil)
        }
        let sampler = pitch ? try texture(gpu, surface, [.shaderRead]) : nil
        return Target(
            surface: surface, texture: try texture(gpu, surface, [.renderTarget, .shaderRead]), sampler: sampler)
    }

    static func texture(_ gpu: GPU, _ surface: IOSurface, _ usage: MTLTextureUsage) throws -> MTLTexture {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: side, height: side, mipmapped: false)
        descriptor.usage = usage
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no texture over the IOSurface")
        }
        return texture
    }
}
