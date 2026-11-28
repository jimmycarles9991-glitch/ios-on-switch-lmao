import CoreVideo
import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal
import VideoToolbox

/// Engine-offload survey 6.1: GPU render -> VIC scale -> GPU sample with no CPU wait between them. A 1280x720 BGRA
/// pixel buffer the CPU filled with `stale` is drawn flat `drawn` by a draw slowed to tens of milliseconds, committed
/// and not waited for; at once a VTPixelTransferSession (TegraVIC's BGRA scale) scales it into a 640x360 buffer the CPU
/// filled with `unwritten`; at once a kernel samples that buffer. Every sample must be `drawn` within 2 codes: a stale
/// sample means VIC read before the GPU's write landed, an unwritten one that the GPU sampled before VIC's write did,
/// another value a torn read. stale, unwritten, torn: samples of each (0); vt_ms: the transfer call; total_ms: from the
/// draw's commit to the sample's completion; draw_gpu_ms: the draw's GPU time. The transfer orders behind the draw only
/// through the kernel (a writer promise or the job's own dependencies: which one a plain commit takes is not read here),
/// so the path counters' writer_promises, promise_waits and promise_cpu_releases deltas are reported beside it. An M4
/// Max's transfer does not wait for the draw: six runs in one process, the first transfer (13 ms, its session starting)
/// saw the draw, each later one (0.7 to 0.9 ms) read the CPU fill at all 230,400 samples; the driver orders it
/// through the kernel, which Apple's does not. A command buffer of the test's own off the driver fails it.
struct X1GpuVicSyncTest: HWTest {
    let id = "gpu.x1.gpu_vic_sync"
    let name = "GPU draw, VIC scale and GPU sample in one go: no stale, unwritten or torn sample"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let (width, height) = (1280, 720)
    static let stale: [UInt8] = [0x20, 0x40, 0x60, 0xff], unwritten: [UInt8] = [0xe0, 0x10, 0x90, 0xff]
    static let drawn = SIMD4<Float>(0.75, 0.5, 0.25, 1)
    static let spin: UInt32 = 1500

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        var metrics = Metrics()
        let source = try Self.buffer(Self.width, Self.height, Self.stale)
        let destination = try Self.buffer(Self.width / 2, Self.height / 2, Self.unwritten)
        let target = try Self.texture(gpu, source)
        let sampled = try Self.texture(gpu, destination)
        var session: VTPixelTransferSession?
        guard VTPixelTransferSessionCreate(allocator: nil, pixelTransferSessionOut: &session) == noErr, let session
        else { return .fail("VTPixelTransferSessionCreate failed") }
        defer { VTPixelTransferSessionInvalidate(session) }
        let state = try gpu.render(fragment: "x1_spun_colour", format: .bgra8Unorm)
        let connection = GPUConnection()
        let counted = Self.promiseCounts(connection)
        let began = DispatchTime.now()
        let draw = try gpu.commit { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            var seeded: (UInt32, UInt32, Float, UInt32) = (0, Self.spin, 0, 0)
            var colour = Self.drawn
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBytes(&seeded, length: 16, index: 0)
            encoder.setFragmentBytes(&colour, length: 16, index: 1)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        let status = VTPixelTransferSessionTransferImage(session, from: source, to: destination)
        let transferred = DispatchTime.now()
        let texels = try await gpu.texels(sampled)
        let ended = DispatchTime.now()
        try gpu.finish([draw])
        if let before = counted, let after = Self.promiseCounts(connection) {
            for key in Self.counters { metrics.set(key, after[key, default: 0] - before[key, default: 0]) }
        }
        let ms = { (a: DispatchTime, b: DispatchTime) in Double(b.uptimeNanoseconds - a.uptimeNanoseconds) / 1e6 }
        metrics.set("vt_ms", ms(began, transferred), digits: 1)
        metrics.set("total_ms", ms(began, ended), digits: 1)
        metrics.set("draw_gpu_ms", max(0, draw.gpuEndTime - draw.gpuStartTime) * 1000, digits: 1)
        guard status == noErr else { return .fail("VTPixelTransferSessionTransferImage: \(status)", metrics) }
        let reading = Self.classify(texels)
        metrics.set("stale", reading.stale)
        metrics.set("unwritten", reading.unwritten)
        metrics.set("torn", reading.torn)
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let offRaw = GM20BFacade.kind(of: gpu.device) == "gm20b" ? gpu.paths.offRaw : PathTally()
        return Outcome.judged(
            [
                ("no sample of the source's CPU fill", reading.stale == 0),
                ("no sample of the destination's CPU fill", reading.unwritten == 0),
                ("no torn sample", reading.torn == 0),
            ], metrics, proof: gpu.proof
        ).failing(offRaw: offRaw)
    }

    static let counters = ["writer_promises", "promise_waits", "promise_cpu_releases"]

    static func promiseCounts(_ connection: GPUConnection?) -> [String: Int]? {
        if case .counts(let paths) = GPUPaths.read(connection) { return paths.counts }
        return nil
    }

    /// RGBA codes of a sampled texel
    static func codes(_ t: SIMD4<Float>) -> [Int] { (0 ..< 4).map { Int((t[$0] * 255).rounded()) } }

    static func near(_ a: [Int], _ b: [Int]) -> Bool { zip(a, b).allSatisfy { abs($0 - $1) <= 2 } }

    static func classify(_ texels: [SIMD4<Float>]) -> (stale: Int, unwritten: Int, torn: Int) {
        let want = (0 ..< 4).map { Int((drawn[$0] * 255).rounded()) }
        let rgba = { (bgra: [UInt8]) in [Int(bgra[2]), Int(bgra[1]), Int(bgra[0]), Int(bgra[3])] }
        var reading = (stale: 0, unwritten: 0, torn: 0)
        for t in texels.map(codes) where !near(t, want) {
            if near(t, rgba(stale)) {
                reading.stale += 1
            } else if near(t, rgba(unwritten)) {
                reading.unwritten += 1
            } else {
                reading.torn += 1
            }
        }
        return reading
    }

    static func buffer(_ width: Int, _ height: Int, _ fill: [UInt8]) throws -> CVPixelBuffer {
        var made: CVPixelBuffer?
        let attributes: [CFString: Any] = [kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary]
        guard
            CVPixelBufferCreate(nil, width, height, kCVPixelFormatType_32BGRA, attributes as CFDictionary, &made)
            == kCVReturnSuccess, let buffer = made
        else { throw TestFailure("no \(width)x\(height) BGRA pixel buffer") }
        CVPixelBufferLockBaseAddress(buffer, [])
        let base = CVPixelBufferGetBaseAddress(buffer)!.assumingMemoryBound(to: UInt8.self)
        for i in 0 ..< (CVPixelBufferGetBytesPerRow(buffer) * height) { base[i] = fill[i % 4] }
        CVPixelBufferUnlockBaseAddress(buffer, [])
        return buffer
    }

    static func texture(_ gpu: GPU, _ buffer: CVPixelBuffer) throws -> MTLTexture {
        guard let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue() else {
            throw TestFailure("a pixel buffer without an IOSurface")
        }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: CVPixelBufferGetWidth(buffer), height: CVPixelBufferGetHeight(buffer),
            mipmapped: false
        )
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no texture over the BGRA surface")
        }
        return texture
    }
}
