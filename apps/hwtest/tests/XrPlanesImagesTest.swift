import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// RGB10A8_2P_XR10 (550) colours at the last colour index and beside a device-made 550, each rendered through its own
/// RGBA16Float stand-in. images: a 64x64 'b3a8' surface's
/// 550 at colour 7 beside seven BGRA8Unorm targets, every colour loaded, a draw of per-texel XR values (colour in
/// [-0.75, 1.25], alpha in [0, 1]) to every colour over pixels [8, 56) x [4, 40): the surface's word plane and alpha
/// plane must equal those of the same draw into another surface's 550 alone, both planes CPU-written alike before, and
/// the lone surface holds the CPU's bytes outside the quad and XRTwoPlaneTest.texel of each value held as a half
/// inside. made: a one-level 550 made from a descriptor at colour 0 beside a surface's 550 at colour 1, both cleared
/// and drawn (texel_values_two): the surface's planes and the device-made one's words, read by a blit, each hold the
/// clear and the draw by the same rule. Tolerance 0. An M4 Max, this scene: 0 texels off in every check.
/// images_word_off, images_alpha_off texels off, images_max the largest difference in codes, alone_off,
/// made_surface_off and made_kept_off texels off; every command buffer raw, failed_cbs=0.
struct XrPlanesImagesTest: HWTest {
    let id = "gpu.gm20b.xr_planes_images"
    let name = "RGB10A8_2P_XR10 colours beside seven targets or a device-made 550 draw into their planes, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let side = 64, quad = (8, 4, 56, 40)
    static let format = XRTwoPlaneTest.format
    static let clear = SIMD4<Float>(-0.25, 0.625, 1.125, 0.5)
    static let prior: [(rect: (Int, Int, Int, Int), word: UInt32, alpha: UInt8)] = [
        ((0, 0, 64, 64), 700 << 20 | 200 << 10 | 450, 128), ((10, 30, 50, 60), 100 << 20 | 900 << 10 | 384, 7),
    ]

    static let values: [SIMD4<Float>] = {
        var state: UInt64 = 550
        return (0 ..< side * side).map { _ in
            let w = WordMipmapCases.splitmix(&state)
            let colour = { (k: UInt64) in Float(w >> (16 * k) % 2001) / 1000 - 0.75 }
            return SIMD4(colour(0), colour(1), colour(2), Float(w >> 48 % 256) / 255)
        }
    }()

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            let (beside, alone) = try await Self.images(gpu)
            let judged = Self.judged(beside, alone)
            let aloneOff = XRTwoPlaneTest.wrongTexels(alone, Self.expected(clear: nil, draw: { $0 }))
            metrics.set("images_word_off", judged.word)
            metrics.set("images_alpha_off", judged.alpha)
            metrics.set("images_max", judged.worst)
            metrics.set("alone_off", aloneOff)
            checks.append(("images: the planes those of the 550 drawn alone", judged.word + judged.alpha == 0))
            checks.append(("images: the 550 drawn alone holds the CPU's bytes and the draw", aloneOff == 0))
        } catch {
            checks.append(("images: \(GPU.describe(error))", false))
        }
        do {
            let (surface, made) = try await Self.made(gpu)
            let off = XRTwoPlaneTest.wrongTexels(surface, Self.expected(clear: Self.clear, draw: Self.turned))
            let drawn = Self.expected(clear: Self.clear, draw: { $0 })
            let kept = drawn.indices.filter { made.loaded(as: UInt32.self, at: $0) != UInt32(drawn[$0] & 0xffff_ffff) }
            metrics.set("made_surface_off", off)
            metrics.set("made_kept_off", kept.count)
            checks.append(("made: the surface's planes hold the clear and the draw", off == 0))
            checks.append(("made: the device-made 550 keeps the clear and the draw", kept.isEmpty))
        } catch {
            checks.append(("made: \(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func turned(_ v: SIMD4<Float>) -> SIMD4<Float> { SIMD4(v.z, v.x, v.y, v.w) }

    static func inside(_ at: Int) -> Bool {
        let (x, y) = (at % side, at / side)
        return x >= quad.0 && x < quad.2 && y >= quad.1 && y < quad.3
    }

    static func expected(clear: SIMD4<Float>?, draw: (SIMD4<Float>) -> SIMD4<Float>) -> [UInt64] {
        var texels = [UInt64](repeating: 0, count: side * side)
        if let clear {
            XRTwoPlaneTest.paint(&texels, (0, 0, side, side), XRTwoPlaneTest.texel(clear), width: side)
        } else {
            for p in prior {
                XRTwoPlaneTest.paint(&texels, p.rect, UInt64(p.alpha) << 32 | UInt64(p.word), width: side)
            }
        }
        let held = { (v: SIMD4<Float>) in XRTwoPlaneTest.texel(SIMD4<Float>(SIMD4<Float16>(v))) }
        return (0 ..< side * side).map { inside($0) ? held(draw(values[$0])) : texels[$0] }
    }

    static func judged(_ a: [UInt64], _ b: [UInt64]) -> (word: Int, alpha: Int, worst: Int) {
        var judged = (word: 0, alpha: 0, worst: 0)
        for (x, y) in zip(a, b) {
            judged.word += x & 0xffff_ffff == y & 0xffff_ffff ? 0 : 1
            judged.alpha += x >> 32 == y >> 32 ? 0 : 1
            for (shift, mask) in [(0, 0x3ff), (10, 0x3ff), (20, 0x3ff), (32, 0xff)] as [(UInt64, UInt64)] {
                let apart = abs(Int(x >> shift & mask) - Int(y >> shift & mask))
                judged.worst = max(judged.worst, apart)
            }
        }
        return judged
    }

    static func surface(_ gpu: GPU, written: Bool) throws -> (CVPixelBuffer, MTLTexture) {
        let (buffer, texture) = try XRTwoPlaneTest.surfaceTexture(gpu, width: side, height: side)
        var scratch = [UInt64](repeating: 0, count: XRTwoPlaneTest.width * XRTwoPlaneTest.height)
        if written { for p in prior { XRTwoPlaneTest.writePlanes(buffer, p, &scratch) } }
        return (buffer, texture)
    }

    static func pass(
        _ gpu: GPU, _ targets: [MTLTexture], fragment: String, load: MTLLoadAction, values: [SIMD4<Float>]
    ) async throws {
        let state = try gpu.render(fragment: fragment, format: targets[0].pixelFormat) { d in
            for (slot, t) in targets.enumerated() { d.colorAttachments[slot].pixelFormat = t.pixelFormat }
        }
        let data = try gpu.buffer(values)
        var width = UInt32(side)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            for (slot, t) in targets.enumerated() {
                pass.colorAttachments[slot].texture = t
                pass.colorAttachments[slot].loadAction = load
                pass.colorAttachments[slot].clearColor = MTLClearColor(
                    red: Double(clear.x), green: Double(clear.y), blue: Double(clear.z), alpha: Double(clear.w)
                )
                pass.colorAttachments[slot].storeAction = .store
            }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.pixelQuad(quad.0, quad.1, quad.2, quad.3, width: side, height: side)
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBuffer(data, offset: 0, index: 0)
            encoder.setFragmentBytes(&width, length: 4, index: 1)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
    }

    static func images(_ gpu: GPU) async throws -> (beside: [UInt64], alone: [UInt64]) {
        let (besideBuffer, beside) = try surface(gpu, written: true)
        let (aloneBuffer, alone) = try surface(gpu, written: true)
        let others = try (0 ..< 7).map { _ in try gpu.texture(.bgra8Unorm, side, side) }
        try await pass(gpu, others + [beside], fragment: "eight_target_values", load: .load, values: values)
        try await pass(gpu, [alone], fragment: "texel_values", load: .load, values: values)
        return (
            XRTwoPlaneTest.planes(besideBuffer, width: side, height: side),
            XRTwoPlaneTest.planes(aloneBuffer, width: side, height: side)
        )
    }

    static func made(_ gpu: GPU) async throws -> (surface: [UInt64], made: [UInt8]) {
        let made = try gpu.texture(format, side, side)
        let (buffer, surface) = try surface(gpu, written: false)
        try await pass(gpu, [made, surface], fragment: "texel_values_two", load: .clear, values: values)
        return (
            XRTwoPlaneTest.planes(buffer, width: side, height: side), try await gpu.read(made, bytesPerPixel: 4)
        )
    }
}
