import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// RGB10A8_2P_XR10 (550) over a 'b3a8' surface, as Core Animation's backdrop blur makes it, drawn in its planes
/// (raw_xr_pass.h: the word into plane 0, the alpha byte into plane 1 as a target of its own): a cleared pass with a
/// draw, a pass that loads it and draws again, and a pass that samples it; then, each after the CPU rewrote part of
/// both planes, a loading pass, a sampling pass and a blit into a buffer; last a blit from a buffer into it. Every
/// command buffer must take the raw path, and the planes must hold Metal's bytes (plane 0 the code word, plane 1 the alpha byte, codes as raw_xr.h writeCode stores
/// them), the samples each code's value as a half, and blits move plane 0's words alone (m2v two_planes.rs); rows of
/// 128-byte multiples (96 wide) are drawn in place. Then XRTwoPlaneCells.
struct XRTwoPlaneTest: HWTest {
    let id = "gpu.raw.xr_two_plane"
    let name = "RGB10A8_2P_XR10 over a b3a8 surface: drawn, loaded, sampled and blitted on the raw path"
    let subsystem = Subsystem.metal

    static let width = 96, height = 64
    static let format = MTLPixelFormat(rawValue: 550)!
    static let clear = SIMD4<Float>(0.5, -0.5, 1.0, 0.75)
    static let draws: [(rect: (Int, Int, Int, Int), color: SIMD4<Float>)] = [
        ((8, 4, 40, 30), SIMD4(1.125, 0.125, -0.625, 0.25)),
        ((30, 20, 90, 60), SIMD4(-0.125, 0.375, 1.375, 1.0)),
        ((0, 40, 24, 64), SIMD4(0.0, -1.0, 2.0, 0.125)),
    ]
    static let cpuWrites: [(rect: (Int, Int, Int, Int), word: UInt32, alpha: UInt8)] = [
        ((50, 2, 70, 12), 1000 << 20 | 3 << 10 | 512, 77),
        ((60, 30, 64, 50), 7 << 20 | 1023 << 10 | 384, 250),
        ((10, 50, 30, 62), 384 << 20 | 640 << 10 | 128, 1),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        let counted = RawFacade.counters(of: gpu.device)
        let (buffer, texture) = try Self.surfaceTexture(gpu)
        let draw = try gpu.render(fragment: "solid_fragment", format: Self.format)
        var want = [UInt64](repeating: Self.texel(Self.clear), count: Self.width * Self.height)
        try await Self.pass(gpu, texture, draw, load: .clear, Self.draws[0], &want)
        try await Self.pass(gpu, texture, draw, load: .load, Self.draws[1], &want)
        let drawn = Self.wrongTexels(Self.planes(buffer), want)
        let sampled = try await Self.wrongSamples(gpu, texture, want)

        Self.writePlanes(buffer, Self.cpuWrites[0], &want)
        try await Self.pass(gpu, texture, draw, load: .load, Self.draws[2], &want)
        let loaded = Self.wrongTexels(Self.planes(buffer), want)
        Self.writePlanes(buffer, Self.cpuWrites[1], &want)
        let resampled = try await Self.wrongSamples(gpu, texture, want)
        Self.writePlanes(buffer, Self.cpuWrites[2], &want)
        let words = try await gpu.read(texture, bytesPerPixel: 4)
        let copiedOut = want.indices.filter { words.loaded(as: UInt32.self, at: $0) != UInt32(want[$0] & 0xffff_ffff) }
            .count
        let incoming = (0..<want.count).map { UInt32(($0 &* 2_654_435_761) & 0x3fff_ffff) }
        try await gpu.upload(incoming.withUnsafeBytes { Array($0) }, to: texture, bytesPerRow: Self.width * 4)
        want = zip(want, incoming).map { $0 & 0xff_0000_0000 | UInt64($1) }
        let copiedIn = Self.wrongTexels(Self.planes(buffer), want)

        let moved = { (name: String) in
            (RawFacade.counters(of: gpu.device)[name] ?? 0) - (counted[name] ?? 0)
        }
        let directShadows = moved("xr_plane_shadows")
        guard let m2v = RawFacade.m2vDevice(of: gpu.device) else {
            throw TestFailure("the facade hands out no m2v device")
        }
        let cells = XRTwoPlaneCells(gpu: gpu, reference: try GPU.make(device: m2v))
        let blend = try await cells.blendWrong()
        let storage = try await cells.fetchWrong(barrier: false)
        let barrier = try await cells.fetchWrong(barrier: true)
        let cellShadows = moved("xr_plane_shadows") - directShadows
        let narrow = try await cells.narrowWrong()
        let narrowShadows = moved("xr_plane_shadows") - directShadows - cellShadows
        let nearest = try await cells.pairSamples(texture, "nearest_fragment")
        let linear = try await cells.pairSamples(texture, "linear_fragment")
        let gatherAlpha = try await cells.gatherWrong(texture, want, alpha: true)
        let gatherRed = try await cells.gatherWrong(texture, want, alpha: false)
        let (referenceBuffer, referenceTexture) = try Self.surfaceTexture(cells.reference)
        Self.copyPlanes(from: buffer, to: referenceBuffer)
        let referenceLinear = try await XRTwoPlaneCells.samples(cells.reference, referenceTexture, "linear_fragment")
        let referenceNearest = try await XRTwoPlaneCells.samples(cells.reference, referenceTexture, "nearest_fragment")
        let nearestApart = XRTwoPlaneCells.halfDistance(nearest.halves, referenceNearest)
        let linearApart = XRTwoPlaneCells.halfDistance(linear.halves, referenceLinear)

        var metrics = Metrics()
        let planePasses = moved("xr_plane_passes")
        metrics.set("plane_passes", planePasses)
        metrics.set("plane_shadows", directShadows)
        metrics.set("cell_shadows", cellShadows)
        metrics.set("narrow_wrong", narrow)
        metrics.set("narrow_shadows", narrowShadows)
        metrics.set("blend_wrong", blend.wrong)
        metrics.set("blend_interlocked", blend.interlocked)
        metrics.set("fetch_storage_wrong", storage.wrong)
        metrics.set("fetch_storage_interlocked", storage.interlocked)
        metrics.set("fetch_barrier_wrong", barrier.wrong)
        metrics.set("fetch_barrier_interlocked", barrier.interlocked)
        metrics.set("fetch_barrier_drawn", barrier.fetched)
        metrics.set("pair_nearest", nearest.paired ? 1 : 0)
        metrics.set("pair_nearest_apart", nearestApart.apart)
        metrics.set("pair_linear", linear.paired ? 1 : 0)
        metrics.set("pair_linear_apart", linearApart.apart)
        metrics.set("pair_linear_steps", linearApart.steps)
        metrics.set("pair_gather", gatherAlpha.paired && gatherRed.paired ? 1 : 0)
        metrics.set("pair_gather_alpha_wrong", gatherAlpha.wrong)
        metrics.set("pair_gather_red_wrong", gatherRed.wrong)
        metrics.set("drawn_wrong", drawn)
        metrics.set("sampled_wrong", sampled)
        metrics.set("loaded_wrong", loaded)
        metrics.set("resampled_wrong", resampled)
        metrics.set("copied_out_wrong", copiedOut)
        metrics.set("copied_in_wrong", copiedIn)
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "raw" }
        return .judged(
            [
                ("every command buffer on the raw path (\(gpu.paths.text))", raw),
                ("both planes hold the clear and the draws", drawn == 0),
                ("the samples are the codes' values", sampled == 0),
                ("a loading pass keeps a CPU write of the planes", loaded == 0),
                ("the samples follow a CPU write of the planes", resampled == 0),
                ("a blit into a buffer gives plane 0's words after a CPU write", copiedOut == 0),
                ("a blit from a buffer writes plane 0 alone", copiedIn == 0),
                ("the passes drew in the planes", planePasses > 0),
                (
                    "rows of 128-byte multiples are drawn in place, beside a memoryless 550 too",
                    directShadows == 0 && cellShadows == 0
                ),
                ("a surface 60 wide is drawn through shadows", narrow == 0 && narrowShadows > 0),
                ("blended draws are the m2v device's", blend.wrong == 0 && blend.interlocked > 0),
                (
                    "Core Animation's blended fills in the storage form are the m2v device's",
                    storage.wrong == 0 && storage.interlocked > 0
                ),
                (
                    "Core Animation's blended fills in the barrier form are the m2v device's",
                    barrier.wrong == 0 && barrier.interlocked == 0 && barrier.fetched > 0
                ),
                (
                    "nearest samples through the header pair are the m2v device's",
                    nearest.paired && nearestApart.apart == 0
                ),
                ("linear samples through the header pair within a half step", linear.paired && linearApart.steps <= 1),
                (
                    "gathers of alpha and red through the header pair are the texels' values",
                    gatherAlpha.paired && gatherRed.paired && gatherAlpha.wrong == 0 && gatherRed.wrong == 0
                ),
            ], metrics, proof: gpu.proof)
    }

    /// Both planes of `from` into `to`, two surfaces of one size, under their locks
    static func copyPlanes(from: CVPixelBuffer, to: CVPixelBuffer) {
        CVPixelBufferLockBaseAddress(from, .readOnly)
        CVPixelBufferLockBaseAddress(to, [])
        defer {
            CVPixelBufferUnlockBaseAddress(to, [])
            CVPixelBufferUnlockBaseAddress(from, .readOnly)
        }
        for plane in 0..<2 {
            let rows = CVPixelBufferGetBytesPerRowOfPlane(from, plane)
            precondition(rows == CVPixelBufferGetBytesPerRowOfPlane(to, plane))
            memcpy(
                CVPixelBufferGetBaseAddressOfPlane(to, plane)!, CVPixelBufferGetBaseAddressOfPlane(from, plane)!,
                rows * CVPixelBufferGetHeightOfPlane(from, plane))
        }
    }

    /// raw_xr.h writeCode: a channel's code, or the alpha byte
    static func code(_ value: Float, alpha: Bool = false) -> UInt32 {
        let v = value.isNaN ? 0 : Double(value)
        if alpha { return UInt32((min(max(v, 0), 1) * 255).rounded(.toNearestOrEven)) }
        let k = Int((min(abs(v), 1.5) * 510).rounded(.toNearestOrEven))
        return UInt32(min(max(v < 0 ? 384 - k : 384 + k, 0), 1023))
    }

    /// A texel as the planes hold it: the alpha byte above the code word
    static func texel(_ c: SIMD4<Float>) -> UInt64 {
        UInt64(code(c.w, alpha: true)) << 32 | UInt64(code(c.x) << 20 | code(c.y) << 10 | code(c.z))
    }

    static func paint(
        _ texels: inout [UInt64], _ r: (Int, Int, Int, Int), _ value: UInt64, width: Int = XRTwoPlaneTest.width
    ) {
        for y in r.1..<r.3 { for x in r.0..<r.2 { texels[y * width + x] = value } }
    }

    static func surfaceTexture(
        _ gpu: GPU, width: Int = XRTwoPlaneTest.width, height: Int = XRTwoPlaneTest.height
    ) throws -> (CVPixelBuffer, MTLTexture) {
        var made: CVPixelBuffer?
        let attributes: [CFString: Any] = [kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary]
        guard
            CVPixelBufferCreate(
                nil, width, height, kCVPixelFormatType_30RGBLE_8A_BiPlanar, attributes as CFDictionary, &made)
                == kCVReturnSuccess, let buffer = made,
            let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue()
        else { throw TestFailure("no b3a8 pixel buffer") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: width, height: height, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no 550 texture over the b3a8 surface")
        }
        return (buffer, texture)
    }

    static func pass(
        _ gpu: GPU, _ texture: MTLTexture, _ pipeline: MTLRenderPipelineState, load: MTLLoadAction,
        _ draw: (rect: (Int, Int, Int, Int), color: SIMD4<Float>), _ want: inout [UInt64]
    ) async throws {
        paint(&want, draw.rect, texel(draw.color))
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = load
            pass.colorAttachments[0].clearColor = MTLClearColor(
                red: Double(clear.x), green: Double(clear.y), blue: Double(clear.z), alpha: Double(clear.w))
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            let r = draw.rect
            GPU.draw(
                encoder, corners: GPU.pixelQuad(r.0, r.1, r.2, r.3, width: width, height: height), color: draw.color)
            encoder.endEncoding()
        }
    }

    /// The planes as texels (alpha byte above the word), through a CPU lock of the surface
    static func planes(
        _ buffer: CVPixelBuffer, width: Int = XRTwoPlaneTest.width, height: Int = XRTwoPlaneTest.height
    ) -> [UInt64] {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        let word = CVPixelBufferGetBaseAddressOfPlane(buffer, 0)!,
            alpha = CVPixelBufferGetBaseAddressOfPlane(buffer, 1)!
        let wordRow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0),
            alphaRow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1)
        return (0..<(width * height)).map { i in
            let (x, y) = (i % width, i / width)
            let w = word.loadUnaligned(fromByteOffset: y * wordRow + x * 4, as: UInt32.self)
            let a = alpha.load(fromByteOffset: y * alphaRow + x, as: UInt8.self)
            return UInt64(a) << 32 | UInt64(w)
        }
    }

    /// A CPU write of both planes under the surface's lock, which moves its seed
    static func writePlanes(
        _ buffer: CVPixelBuffer, _ texel: (rect: (Int, Int, Int, Int), word: UInt32, alpha: UInt8),
        _ want: inout [UInt64]
    ) {
        let r = texel.rect
        paint(&want, r, UInt64(texel.alpha) << 32 | UInt64(texel.word))
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        let word = CVPixelBufferGetBaseAddressOfPlane(buffer, 0)!,
            alpha = CVPixelBufferGetBaseAddressOfPlane(buffer, 1)!
        let wordRow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0),
            alphaRow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1)
        for y in r.1..<r.3 {
            for x in r.0..<r.2 {
                word.storeBytes(of: texel.word, toByteOffset: y * wordRow + x * 4, as: UInt32.self)
                alpha.storeBytes(of: texel.alpha, toByteOffset: y * alphaRow + x, as: UInt8.self)
            }
        }
    }

    static func wrongTexels(_ got: [UInt64], _ want: [UInt64]) -> Int {
        zip(got, want).filter { $0 != $1 }.count
    }

    /// Texels whose nearest sample, drawn into an RGBA16Float target, is not each code's value as the nearest half
    static func wrongSamples(_ gpu: GPU, _ texture: MTLTexture, _ want: [UInt64]) async throws -> Int {
        let target = try gpu.texture(.rgba16Float, width, height)
        let pipeline = try gpu.render(vertex: "textured_vertex", fragment: "nearest_fragment", format: .rgba16Float)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            var corners = GPU.fullScreen
            var uvs: [SIMD2<Float>] = [SIMD2(0, 1), SIMD2(1, 1), SIMD2(0, 0), SIMD2(1, 0)]
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setVertexBytes(&uvs, length: MemoryLayout<SIMD2<Float>>.stride * 4, index: 1)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        let bytes = try await gpu.read(target, bytesPerPixel: 8)
        let value = { (n: Int, d: Double) in Float16(Double(n) / d).bitPattern }
        return (0..<(width * height)).filter { i in
            let t = want[i]
            let expected: [UInt16] = [
                value(Int(t >> 20 & 0x3ff) - 384, 510), value(Int(t >> 10 & 0x3ff) - 384, 510),
                value(Int(t & 0x3ff) - 384, 510), value(Int(t >> 32 & 0xff), 255),
            ]
            let got = (0..<4).map { c in bytes.loaded(as: UInt16.self, at: i * 4 + c) }
            return got != expected
        }.count
    }
}
