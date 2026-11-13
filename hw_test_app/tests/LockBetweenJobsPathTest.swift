import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A CPU lock of an IOSurface while a command buffer of several jobs draws into it (TegraGM20BJobABI.h admission:
/// Begin, Continue, End). On Apple's GPUs a lock waits for the GPU work submitted before it, and a command buffer is
/// submitted whole, so no lock reads one half drawn. The command buffer draws one colour over the whole surface, then
/// another: more draws than a job's 129 batches of 128 take, so it goes as several jobs, which the job count shows. A
/// thread locks the surface read-only over and over while it is submitted and reads two pixels; a lock that saw the first
/// colour got between the command buffer's jobs.
struct LockBetweenJobsPathTest: GPUPathTest {
    let id = "gpu.path.lock_between_jobs"
    let name = "A CPU lock taken while a command buffer of several jobs runs never sees it half drawn"
    let counters = ["submit_jobs"]
    static let rounds = 6, draws = 18_000, side = 64, rowBytes = 256
    static let start: [UInt8] = [0x40, 0x40, 0x40, 0xff]
    static let first: [UInt8] = [0, 0, 0xff, 0xff]  // red, as BGRA bytes
    static let last: [UInt8] = [0xff, 0, 0, 0xff]  // blue

    /// What the locking thread saw, written by it and read once it stopped.
    final class Seen: @unchecked Sendable {
        private let lock = NSLock()
        private var running = true
        private(set) var locks = 0, halfDrawn = 0

        var isRunning: Bool {
            lock.lock()
            defer { lock.unlock() }
            return running
        }

        func stop() {
            lock.lock()
            running = false
            lock.unlock()
        }

        func note(_ pixels: [[UInt8]]) {
            lock.lock()
            locks += 1
            if pixels.contains(LockBetweenJobsPathTest.first) { halfDrawn += 1 }
            lock.unlock()
        }
    }

    static func pixel(_ surface: IOSurface, _ x: Int, _ y: Int) -> [UInt8] {
        Array(UnsafeRawBufferPointer(start: surface.baseAddress + y * rowBytes + x * 4, count: 4))
    }

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: Self.side, .height: Self.side, .bytesPerElement: 4, .bytesPerRow: Self.rowBytes,
            .allocSize: Self.rowBytes * Self.side, .pixelFormat: 0x4247_5241,
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface creation failed") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: Self.side, height: Self.side, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no texture over the IOSurface")
        }
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let connection = GPUConnection()
        guard case .counts(let before) = GPUPaths.read(connection) else { throw TestFailure("no path counters") }
        var locks = 0, halfDrawn = 0
        for _ in 0..<Self.rounds {
            _ = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = texture
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.25, green: 0.25, blue: 0.25, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
            let seen = Seen()
            let stopped = DispatchSemaphore(value: 0)
            let locker = Thread {
                while seen.isRunning {
                    if surface.lock(options: .readOnly, seed: nil) == 0 {
                        let pixels = [Self.pixel(surface, 0, 0), Self.pixel(surface, Self.side - 1, Self.side - 1)]
                        surface.unlock(options: .readOnly, seed: nil)
                        seen.note(pixels)
                    }
                    // A Begin refused for a held lock is encoded again once readiness wakes it; a lock taken that
                    // rarely leaves the retry free and still lands across each command buffer's run.
                    usleep(1000)
                }
                stopped.signal()
            }
            locker.start()
            defer {
                seen.stop()
                stopped.wait()
                locks += seen.locks
                halfDrawn += seen.halfDrawn
            }
            let draw = try gpu.commit { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = texture
                pass.colorAttachments[0].loadAction = .load
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                var corners = GPU.pixelQuad(0, 0, Self.side, Self.side, width: Self.side, height: Self.side)
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                for colour in [[Float(1), 0, 0, 1], [Float(0), 0, 1, 1]] {
                    var rgba = colour
                    encoder.setFragmentBytes(&rgba, length: MemoryLayout<Float>.stride * 4, index: 0)
                    for _ in 0..<Self.draws {
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    }
                }
                encoder.endEncoding()
            }
            try gpu.finish([draw])
        }
        guard case .counts(let after) = GPUPaths.read(connection) else { throw TestFailure("no path counters after") }
        let jobs = after.taken("submit_jobs", since: before)
        guard surface.lock(options: .readOnly, seed: nil) == 0 else { throw TestFailure("IOSurfaceLock failed") }
        var wrong = 0
        for y in 0..<Self.side {
            for x in 0..<Self.side where Self.pixel(surface, x, y) != Self.last { wrong += 1 }
        }
        surface.unlock(options: .readOnly, seed: nil)
        metrics.set("rounds", Self.rounds)
        metrics.set("locks", locks)
        metrics.set("half_drawn", halfDrawn)
        metrics.set("wrong_pixels", wrong)
        var failures: [String] = []
        if halfDrawn != 0 { failures.append("\(halfDrawn) locks read a command buffer half drawn") }
        if wrong != 0 { failures.append("\(wrong) pixels miss the last draws") }
        // each round: its clear's job, and three or more for the draws
        if jobs < 4 * Self.rounds {
            failures.append("\(jobs) jobs in \(Self.rounds) rounds: the draws went as fewer than three a round")
        }
        if locks == 0 { failures.append("no lock was taken while the command buffers ran") }
        return failures
    }
}
