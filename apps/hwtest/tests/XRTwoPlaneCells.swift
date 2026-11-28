import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.gm20b.xr_two_plane's cells beyond the planes' own bytes, each against the CPU model (XRTwoPlaneModel): blended
/// draws of the app's own state (made on the device, blended by the ROP in the stand-in, no interlock), Core Animation's
/// source-over of its framebuffer fetch in the pack record's storage form and in its barrier form (probe_forms.h fetch=barrier), each within one code, a surface
/// whose alpha rows miss a pitch target's 128 bytes, and samples through the texture's header pair
/// (nvk_switch_xr_planes.h): nearest exact, linear within a half step of Metal's filter, and gathers of alpha and red.
final class XRTwoPlaneCells {
    let gpu: GPU
    private var cachedFetchPipeline: MTLRenderPipelineState?

    init(gpu: GPU) { self.gpu = gpu }

    static let blends: [(rect: (Int, Int, Int, Int), color: SIMD4<Float>)] = [
        ((4, 4, 60, 40), SIMD4(0.25, 0.5, -0.125, 0.5)),
        ((20, 16, 92, 60), SIMD4(0.75, -0.25, 1.0, 0.75)),
        ((40, 0, 70, 64), SIMD4(0.0, 0.0, 0.0, 0.0)),
    ]

    /// The planes after the blended draws against the model (texels beyond one code, and exactly one off), and the
    /// device's interlocked draws
    func blendWrong() async throws -> (wrong: Int, offByOne: Int, interlocked: Int) {
        let (width, height) = (XRTwoPlaneTest.width, XRTwoPlaneTest.height)
        let (buffer, texture) = try XRTwoPlaneTest.surfaceTexture(gpu)
        let draw = try gpu.render(fragment: "solid_half_fragment", format: XRTwoPlaneTest.format) { d in
            let a = d.colorAttachments[0]!
            a.isBlendingEnabled = true
            a.sourceRGBBlendFactor = .one
            a.destinationRGBBlendFactor = .oneMinusSourceAlpha
            a.sourceAlphaBlendFactor = .one
            a.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        }
        let before = GM20BFacade.counters(of: gpu.device)["interlock_draws"] ?? 0
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            let c = XRTwoPlaneTest.clear
            pass.colorAttachments[0].clearColor = MTLClearColor(
                red: Double(c.x), green: Double(c.y), blue: Double(c.z), alpha: Double(c.w)
            )
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(draw)
            for b in Self.blends {
                let r = b.rect
                GPU.draw(
                    encoder, corners: GPU.pixelQuad(r.0, r.1, r.2, r.3, width: width, height: height), color: b.color
                )
            }
            encoder.endEncoding()
        }
        let interlocked = (GM20BFacade.counters(of: gpu.device)["interlock_draws"] ?? 0) - before
        var want = [UInt64](repeating: XRTwoPlaneTest.texel(XRTwoPlaneTest.clear), count: width * height)
        for b in Self.blends {
            let half = SIMD4<Float16>(b.color)
            let source = SIMD4<Double>(Double(half.x), Double(half.y), Double(half.z), Double(half.w))
            let r = b.rect
            for y in r.1 ..< r.3 {
                for x in r.0 ..< r.2 { want[y * width + x] = XRTwoPlaneModel.over(source, want[y * width + x]) }
            }
        }
        let (wrong, offByOne) = XRTwoPlaneModel.apart(XRTwoPlaneTest.planes(buffer), want)
        return (wrong, offByOne, interlocked)
    }

    /// Core Animation's untextured source-over fill into 'b3a8' beside its memoryless colours 1 and 2, as this OS's
    /// QuartzCore makes it (CACorpus): the pack's record fetches the 550 colour's stand-in and blends over it in the
    /// program (its storage form, and its barrier twin)
    static let fetchLabel = "Pb3a8BsovXmw_A3Xhfx"

    static let fetches: [(x0: Float, y0: Float, x1: Float, y1: Float, color: SIMD4<Float16>)] = [
        (-0.9, -0.8, 0.3, 0.5, SIMD4(0.25, 0.5, -0.125, 0.5)),
        (-0.4, -0.2, 0.9, 0.9, SIMD4(0.75, -0.25, 1.0, 0.75)),
        (0.1, -1, 0.5, 1, SIMD4(0, 0, 0, 0)),
    ]

    /// The planes after the blended fills against the model (texels beyond one code, and exactly one off); `barrier`:
    /// the pack record's barrier form. The device's interlocked draws and its barrier form's fetch draws (drawn whole or
    /// a triangle run at a time).
    func fetchWrong(barrier: Bool) async throws -> (wrong: Int, offByOne: Int, interlocked: Int, fetched: Int) {
        let (width, height) = (XRTwoPlaneTest.width, XRTwoPlaneTest.height)
        guard let line = CACorpus.state(Self.fetchLabel) else {
            throw TestFailure("CACorpus has no \(Self.fetchLabel)")
        }
        let pipeline: MTLRenderPipelineState
        if let cached = cachedFetchPipeline {
            pipeline = cached
        } else {
            let library = try gpu.device.makeLibrary(URL: CAState.library)
            pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
            cachedFetchPipeline = pipeline
        }
        let (buffer, texture) = try XRTwoPlaneTest.surfaceTexture(gpu)
        let scratch = try [1, 2].map {
            try gpu.texture(CACorpus.colorFormat(line, $0), width, height, usage: [.renderTarget], storage: .memoryless)
        }
        let before = GM20BFacade.counters(of: gpu.device)
        try await gpu.run { commands in
            if barrier { try Self.forms(commands, "fetch=barrier") }
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            let c = XRTwoPlaneTest.clear
            pass.colorAttachments[0].clearColor = MTLClearColor(
                red: Double(c.x), green: Double(c.y), blue: Double(c.z), alpha: Double(c.w)
            )
            pass.colorAttachments[0].storeAction = .store
            for (index, texture) in scratch.enumerated() {
                pass.colorAttachments[index + 1].texture = texture
                pass.colorAttachments[index + 1].loadAction = .clear
                pass.colorAttachments[index + 1].storeAction = .dontCare
            }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            for f in Self.fetches {
                CAState.bind(
                    encoder, vertices: CAState.rectangle(x0: f.x0, y0: f.y0, x1: f.x1, y1: f.y1, color: f.color),
                    source: nil
                )
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
        let after = GM20BFacade.counters(of: gpu.device)
        let moved = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }
        var want = [UInt64](repeating: XRTwoPlaneTest.texel(XRTwoPlaneTest.clear), count: width * height)
        for f in Self.fetches {
            let source = SIMD4<Double>(Double(f.color.x), Double(f.color.y), Double(f.color.z), Double(f.color.w))
            for i in want.indices
                where XRTwoPlaneModel.covers(
                    i % width, i / width, x0: f.x0, y0: f.y0, x1: f.x1, y1: f.y1, width: width, height: height
                )
            {
                want[i] = XRTwoPlaneModel.over(source, want[i])
            }
        }
        let (wrong, offByOne) = XRTwoPlaneModel.apart(XRTwoPlaneTest.planes(buffer), want)
        return (wrong, offByOne, moved("interlock_draws"), moved("fetch_sequential"))
    }

    /// Texels wrong after a clear and a draw into a surface 60 wide: its alpha plane's rows (64 bytes) miss the 128 a
    /// pitch target's address and rows take (gpu.cpp linearTarget), so the pass draws its planes through shadows
    func narrowWrong() async throws -> Int {
        let (width, height) = (60, 40)
        let (buffer, texture) = try XRTwoPlaneTest.surfaceTexture(gpu, width: width, height: height)
        let draw = try gpu.render(fragment: "solid_fragment", format: XRTwoPlaneTest.format)
        let c = XRTwoPlaneTest.clear
        var want = [UInt64](repeating: XRTwoPlaneTest.texel(c), count: width * height)
        let r = (6, 4, 50, 33), color = SIMD4<Float>(1.125, -0.25, 0.375, 0.625)
        XRTwoPlaneTest.paint(&want, r, XRTwoPlaneTest.texel(color), width: width)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(
                red: Double(c.x), green: Double(c.y), blue: Double(c.z), alpha: Double(c.w)
            )
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(draw)
            GPU.draw(encoder, corners: GPU.pixelQuad(r.0, r.1, r.2, r.3, width: width, height: height), color: color)
            encoder.endEncoding()
        }
        return XRTwoPlaneTest.wrongTexels(XRTwoPlaneTest.planes(buffer, width: width, height: height), want)
    }

    static func drawSample(
        _ gpu: GPU, _ texture: MTLTexture, target: MTLTexture, pipeline: MTLRenderPipelineState, shift: Float
    ) async throws {
        let (width, height) = (XRTwoPlaneTest.width, XRTwoPlaneTest.height)
        let du = shift / Float(width), dv = shift / Float(height)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            var corners = GPU.fullScreen
            var uvs: [SIMD2<Float>] = [SIMD2(du, 1 + dv), SIMD2(1 + du, 1 + dv), SIMD2(du, dv), SIMD2(1 + du, dv)]
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setVertexBytes(&uvs, length: MemoryLayout<SIMD2<Float>>.stride * 4, index: 1)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
    }

    static func readHalves(_ gpu: GPU, _ target: MTLTexture) async throws -> [UInt16] {
        let bytes = try await gpu.read(target, bytesPerPixel: 8)
        let count = bytes.count / 2
        var halves = [UInt16](repeating: 0, count: count)
        bytes.withUnsafeBytes { raw in
            halves.withUnsafeMutableBufferPointer { dst in
                for i in 0 ..< count { dst[i] = raw.loadUnaligned(fromByteOffset: i * 2, as: UInt16.self) }
            }
        }
        return halves
    }

    /// Draws each pair-sampled fragment once up front so the facade compiles all four sampled variants in parallel.
    func warmPairPipelines(_ texture: MTLTexture) async throws -> [String: MTLRenderPipelineState] {
        let names = ["nearest_fragment", "linear_fragment", "gather_alpha_fragment", "gather_red_fragment"]
        var pipelines: [String: MTLRenderPipelineState] = [:]
        for name in names {
            pipelines[name] = try gpu.render(vertex: "textured_vertex", fragment: name, format: .rgba16Float)
        }
        let target = try gpu.texture(.rgba16Float, XRTwoPlaneTest.width, XRTwoPlaneTest.height)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            var corners = GPU.fullScreen
            var uvs: [SIMD2<Float>] = [SIMD2(0, 1), SIMD2(1, 1), SIMD2(0, 0), SIMD2(1, 0)]
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setVertexBytes(&uvs, length: MemoryLayout<SIMD2<Float>>.stride * 4, index: 1)
            encoder.setFragmentTexture(texture, index: 0)
            for name in names {
                encoder.setRenderPipelineState(pipelines[name]!)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
        return pipelines
    }

    /// Samples drawn through the header pair: the state's sampled variant is made on the device at its first draw
    /// (TegraGM20BMetalCubic.mm renderVariant), its stand-in drawn until then, so the samples are taken again until the
    /// facade's header-pair binds move, up to two seconds
    func pairSamples(
        _ texture: MTLTexture, _ fragment: String, shift: Float = 1.0 / 3,
        warmed: MTLRenderPipelineState? = nil
    ) async throws -> (halves: [UInt16], paired: Bool) {
        let target = try gpu.texture(.rgba16Float, XRTwoPlaneTest.width, XRTwoPlaneTest.height)
        let pipeline = try warmed ?? gpu.render(vertex: "textured_vertex", fragment: fragment, format: .rgba16Float)
        for _ in 0 ..< 200 {
            let before = GM20BFacade.counters(of: gpu.device)["xr_pair_binds"] ?? 0
            try await Self.drawSample(gpu, texture, target: target, pipeline: pipeline, shift: shift)
            if (GM20BFacade.counters(of: gpu.device)["xr_pair_binds"] ?? 0) > before {
                return (try await Self.readHalves(gpu, target), true)
            }
            try await Task.sleep(nanoseconds: 10_000_000)
        }
        try await Self.drawSample(gpu, texture, target: target, pipeline: pipeline, shift: shift)
        return (try await Self.readHalves(gpu, target), false)
    }

    @inline(__always)
    private static func sort4(_ a: UInt16, _ b: UInt16, _ c: UInt16, _ d: UInt16) -> (UInt16, UInt16, UInt16, UInt16) {
        var x0 = a, x1 = b, x2 = c, x3 = d
        if x0 > x1 { swap(&x0, &x1) }
        if x2 > x3 { swap(&x2, &x3) }
        if x0 > x2 { swap(&x0, &x2) }
        if x1 > x3 { swap(&x1, &x3) }
        if x1 > x2 { swap(&x1, &x2) }
        return (x0, x1, x2, x3)
    }

    /// Pixels whose gather through the header pair (of alpha, else of red: the header's swizzle gives RGB10A2's third
    /// field, the red code, as red) is not, in any order, the values of the four texels `want` holds about its texel
    /// corner: each byte over 255, each code's (c - 384) / 510, as halves
    func gatherWrong(
        _ texture: MTLTexture, _ want: [UInt64], alpha: Bool, warmed: MTLRenderPipelineState? = nil
    ) async throws -> (wrong: Int, paired: Bool) {
        let fragment = alpha ? "gather_alpha_fragment" : "gather_red_fragment"
        let (halves, paired) = try await pairSamples(texture, fragment, shift: 0.5, warmed: warmed)
        let w = XRTwoPlaneTest.width
        @inline(__always) func value(_ t: UInt64) -> UInt16 {
            alpha
                ? Float16(Double(t >> 32 & 0xff) / 255).bitPattern
                : Float16(Double(Int(t >> 20 & 0x3ff) - 384) / 510).bitPattern
        }
        var wrong = 0
        for y in 0 ..< (XRTwoPlaneTest.height - 1) {
            for x in 0 ..< (w - 1) {
                let i = y * w + x
                let g = Self.sort4(halves[i * 4], halves[i * 4 + 1], halves[i * 4 + 2], halves[i * 4 + 3])
                let a = Self.sort4(value(want[i]), value(want[i + 1]), value(want[i + w]), value(want[i + w + 1]))
                if g != a { wrong += 1 }
            }
        }
        return (wrong, paired)
    }

    /// probe_forms.h: the raw command buffer's barrier forms (debug builds' TegraGM20BMetalProbe.mm)
    static func forms(_ commands: MTLCommandBuffer, _ forms: String) throws {
        let selector = NSSelectorFromString("switchMetalProbeForms:")
        guard let object = commands as? NSObject, object.responds(to: selector) else {
            throw TestFailure("the facade has no switchMetalProbeForms: (a release plug-in)")
        }
        if let refusal = object.perform(selector, with: forms)?.takeUnretainedValue() as? String {
            throw TestFailure(refusal)
        }
    }
}
