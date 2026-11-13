import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.xr_two_plane's cells beyond the planes' own bytes, each against the same work on the m2v device: blended
/// draws of the app's own state (made on the device, its storage form alone), Core Animation's source-over in the pack
/// record's storage form and in its barrier twin (raw_probe_forms.h fetch=barrier), a surface whose alpha rows miss a
/// pitch target's 128 bytes, and samples through the texture's header pair (nvk_switch_xr_planes.h), nearest exact,
/// linear (Metal's filter, emulated on the m2v device) and gathers of alpha and red against the texels' values.
struct XRTwoPlaneCells {
    let gpu: GPU
    let reference: GPU

    static let blends: [(rect: (Int, Int, Int, Int), color: SIMD4<Float>)] = [
        ((4, 4, 60, 40), SIMD4(0.25, 0.5, -0.125, 0.5)),
        ((20, 16, 92, 60), SIMD4(0.75, -0.25, 1.0, 0.75)),
        ((40, 0, 70, 64), SIMD4(0.0, 0.0, 0.0, 0.0)),
    ]

    /// Texels where the planes after the blended draws differ from the m2v device's, and the raw device's interlocked
    /// draws
    func blendWrong() async throws -> (wrong: Int, interlocked: Int) {
        var planes: [[UInt64]] = []
        var interlocked = 0
        for (index, g) in [gpu, reference].enumerated() {
            let (buffer, texture) = try XRTwoPlaneTest.surfaceTexture(g)
            let draw = try g.render(fragment: "solid_half_fragment", format: XRTwoPlaneTest.format) { d in
                let a = d.colorAttachments[0]!
                a.isBlendingEnabled = true
                a.sourceRGBBlendFactor = .one
                a.destinationRGBBlendFactor = .oneMinusSourceAlpha
                a.sourceAlphaBlendFactor = .one
                a.destinationAlphaBlendFactor = .oneMinusSourceAlpha
            }
            let before = RawFacade.counters(of: g.device)["interlock_draws"] ?? 0
            try await g.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = texture
                pass.colorAttachments[0].loadAction = .clear
                let c = XRTwoPlaneTest.clear
                pass.colorAttachments[0].clearColor = MTLClearColor(
                    red: Double(c.x), green: Double(c.y), blue: Double(c.z), alpha: Double(c.w))
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(draw)
                for b in Self.blends {
                    let r = b.rect
                    GPU.draw(
                        encoder,
                        corners: GPU.pixelQuad(
                            r.0, r.1, r.2, r.3, width: XRTwoPlaneTest.width, height: XRTwoPlaneTest.height),
                        color: b.color)
                }
                encoder.endEncoding()
            }
            if index == 0 { interlocked = (RawFacade.counters(of: g.device)["interlock_draws"] ?? 0) - before }
            planes.append(XRTwoPlaneTest.planes(buffer))
        }
        return (XRTwoPlaneTest.wrongTexels(planes[0], planes[1]), interlocked)
    }

    /// Core Animation's untextured source-over fill into 'b3a8' beside its memoryless 550 and R32Float, verbatim from the
    /// corpus: the pack's record fetches the 550 colour and blends over it in the program (its storage form, and its
    /// barrier twin), both planes' (" | xr2p")
    static let fetchLine =
        "R ok=1 label=com.apple.coreanimation.Pb3a8BsovXmw_A3Xhfx samples=1 topo=0 ds=0/0"
        + " c0=550:15:1:1:5:0:1:5:0 c1=550:0:0:1:0:0:1:0:0 c2=55:0:0:1:0:0:1:0:0"
        + " a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1"
        + " | v fixed_vert_lph_spc lib=/System/Library/Frameworks/QuartzCore.framework/default.metallib"
        + " fc_generic:53:00 fc_texcoord_count0:49:00 fc_texcoord_count1:49:00 fc_vertex_layout:49:00"
        + " | f fixed_frag_lph_cph lib=/System/Library/Frameworks/QuartzCore.framework/default.metallib"
        + " fc_attachment_count:49:03 fc_blend_function:49:3a fc_clamp_color:53:00 fc_color_inputs0:49:00"
        + " fc_color_inputs1:49:00 fc_color_opaque0:53:00 fc_color_opaque1:53:00 fc_color_opcodes0:52:00000000"
        + " fc_color_opcodes1:52:00000000 fc_coord_function0:49:00 fc_coord_function1:49:00 fc_debug_xr:53:00"
        + " fc_dest_count:49:00 fc_dest_index:49:00 fc_destination_function:49:00 fc_extended_range:53:01"
        + " fc_filter0:49:00 fc_filter1:49:00 fc_framebuffer_fetch:53:01 fc_generic:53:00"
        + " fc_has_asg_77_weights:53:00 fc_has_attachment_0:53:00 fc_has_attachment_1:53:00"
        + " fc_has_attachment_2:53:00 fc_has_color_cube0:53:00 fc_has_color_cube1:53:00 fc_has_color_params:53:00"
        + " fc_has_color_trc0:53:00 fc_has_color_trc1:53:00 fc_has_edr_factor:53:00 fc_has_gamma_luts:53:00"
        + " fc_has_hdr_scale:53:00 fc_has_image_0A:53:00 fc_has_image_0B:53:00 fc_has_image_0C:53:00"
        + " fc_has_image_1A:53:00 fc_has_image_1B:53:00 fc_has_image_1C:53:00 fc_has_lod_bias:53:00"
        + " fc_has_lut_tex:53:00 fc_has_noise_scale:53:00 fc_has_noise_tex:53:00 fc_has_premultiply:53:00"
        + " fc_has_ycbcr_matrix:53:00 fc_image_count:49:00 fc_image_function0:49:00 fc_image_function1:49:00"
        + " fc_memoryless_index0:49:00 fc_memoryless_index1:49:00 fc_noise_width:49:00 fc_plane_count0:49:00"
        + " fc_plane_count1:49:00 fc_texcoord_count0:49:00 fc_texcoord_count1:49:00 fc_texture_function:49:00"
        + " fc_unbounded_range:53:00 fc_vertex_layout:49:00 | "

    static let fetches: [(x0: Float, y0: Float, x1: Float, y1: Float, color: SIMD4<Float16>)] = [
        (-0.9, -0.8, 0.3, 0.5, SIMD4(0.25, 0.5, -0.125, 0.5)),
        (-0.4, -0.2, 0.9, 0.9, SIMD4(0.75, -0.25, 1.0, 0.75)),
        (0.1, -1, 0.5, 1, SIMD4(0, 0, 0, 0)),
    ]

    /// Texels where the planes after the blended fills differ from the m2v device's; `barrier`: the pack record's barrier
    /// twin. The raw device's interlocked draws and its barrier form's fetch draws (drawn whole or a
    /// triangle run at a time).
    func fetchWrong(barrier: Bool) async throws -> (wrong: Int, interlocked: Int, fetched: Int) {
        guard let line = PipelineLine(Self.fetchLine) else {
            throw TestFailure("the fetch state's line does not parse")
        }
        var planes: [[UInt64]] = []
        var interlocked = 0, fetched = 0
        for (index, g) in [gpu, reference].enumerated() {
            let library = try g.device.makeLibrary(URL: CAState.library)
            let pipeline = try CAState.pipeline(line, library: library, device: g.device)
            let (buffer, texture) = try XRTwoPlaneTest.surfaceTexture(g)
            let scratch = try [XRTwoPlaneTest.format, .r32Float].map {
                try g.texture(
                    $0, XRTwoPlaneTest.width, XRTwoPlaneTest.height, usage: [.renderTarget], storage: .memoryless)
            }
            let before = RawFacade.counters(of: g.device)
            try await g.run { commands in
                if index == 0 && barrier { try Self.forms(commands, "fetch=barrier") }
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = texture
                pass.colorAttachments[0].loadAction = .clear
                let c = XRTwoPlaneTest.clear
                pass.colorAttachments[0].clearColor = MTLClearColor(
                    red: Double(c.x), green: Double(c.y), blue: Double(c.z), alpha: Double(c.w))
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
                        source: nil)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            if index == 0 {
                let after = RawFacade.counters(of: g.device)
                let moved = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }
                interlocked = moved("interlock_draws")
                fetched = moved("fetch_whole") + moved("fetch_sequential")
            }
            planes.append(XRTwoPlaneTest.planes(buffer))
        }
        return (XRTwoPlaneTest.wrongTexels(planes[0], planes[1]), interlocked, fetched)
    }

    /// Texels wrong after a clear and a draw into a surface 60 wide: its alpha plane's rows (64 bytes) miss the 128 a
    /// pitch target's address and rows take (raw_gpu.cpp linearTarget), so the pass draws its planes through shadows
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
                red: Double(c.x), green: Double(c.y), blue: Double(c.z), alpha: Double(c.w))
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(draw)
            GPU.draw(encoder, corners: GPU.pixelQuad(r.0, r.1, r.2, r.3, width: width, height: height), color: color)
            encoder.endEncoding()
        }
        return XRTwoPlaneTest.wrongTexels(XRTwoPlaneTest.planes(buffer, width: width, height: height), want)
    }

    /// The half bits of every texel's sample of `texture` through `fragment` (nearest_fragment, linear_fragment,
    /// gather_*_fragment) into an RGBA16Float target, the uvs moved on by `shift` of a texel (a third: a linear sample
    /// mixes neighbours; a half: a gather reads the four about the texel's far corner)
    static func samples(
        _ gpu: GPU, _ texture: MTLTexture, _ fragment: String, shift: Float = 1.0 / 3
    ) async throws -> [UInt16] {
        let (width, height) = (XRTwoPlaneTest.width, XRTwoPlaneTest.height)
        let target = try gpu.texture(.rgba16Float, width, height)
        let pipeline = try gpu.render(vertex: "textured_vertex", fragment: fragment, format: .rgba16Float)
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
        let bytes = try await gpu.read(target, bytesPerPixel: 8)
        return (0..<(width * height * 4)).map { bytes.loaded(as: UInt16.self, at: $0) }
    }

    /// Samples drawn through the header pair: the state's sampled variant is made on the device at its first draw
    /// (RawMetalCubic.mm renderVariant), its stand-in drawn until then, so the samples are taken again until the
    /// facade's header-pair binds move, up to two seconds
    func pairSamples(
        _ texture: MTLTexture, _ fragment: String, shift: Float = 1.0 / 3
    ) async throws -> (halves: [UInt16], paired: Bool) {
        for _ in 0..<200 {
            let before = RawFacade.counters(of: gpu.device)["xr_pair_binds"] ?? 0
            let halves = try await Self.samples(gpu, texture, fragment, shift: shift)
            if (RawFacade.counters(of: gpu.device)["xr_pair_binds"] ?? 0) > before { return (halves, true) }
            try await Task.sleep(nanoseconds: 10_000_000)
        }
        return (try await Self.samples(gpu, texture, fragment, shift: shift), false)
    }

    /// Pixels whose gather through the header pair (of alpha, else of red: the header's swizzle gives RGB10A2's third
    /// field, the red code, as red) is not, in any order, the values of the four texels `want` holds about its texel
    /// corner: each byte over 255, each code's (c - 384) / 510, as halves
    func gatherWrong(_ texture: MTLTexture, _ want: [UInt64], alpha: Bool) async throws -> (wrong: Int, paired: Bool) {
        let fragment = alpha ? "gather_alpha_fragment" : "gather_red_fragment"
        let (halves, paired) = try await pairSamples(texture, fragment, shift: 0.5)
        let w = XRTwoPlaneTest.width
        let value = { (t: UInt64) -> UInt16 in
            alpha
                ? Float16(Double(t >> 32 & 0xff) / 255).bitPattern
                : Float16(Double(Int(t >> 20 & 0x3ff) - 384) / 510).bitPattern
        }
        var wrong = 0
        for y in 0..<(XRTwoPlaneTest.height - 1) {
            for x in 0..<(w - 1) {
                let i = y * w + x
                let got = (0..<4).map { halves[i * 4 + $0] }.sorted()
                let around = [want[i], want[i + 1], want[i + w], want[i + w + 1]].map(value).sorted()
                if got != around { wrong += 1 }
            }
        }
        return (wrong, paired)
    }

    /// Lanes apart, and the largest distance between two halves in steps of their bits (both of one sign)
    static func halfDistance(_ a: [UInt16], _ b: [UInt16]) -> (apart: Int, steps: Int) {
        var apart = 0, steps = 0
        for (x, y) in zip(a, b) where x != y {
            apart += 1
            let order = { (h: UInt16) in h & 0x8000 != 0 ? -Int(h & 0x7fff) : Int(h) }
            steps = max(steps, abs(order(x) - order(y)))
        }
        return (apart, steps)
    }

    /// raw_probe_forms.h: the raw command buffer's barrier forms (debug builds' RawMetalProbe.mm)
    static func forms(_ commands: MTLCommandBuffer, _ forms: String) throws {
        let selector = NSSelectorFromString("switchRawProbeForms:")
        guard let object = commands as? NSObject, object.responds(to: selector) else {
            throw TestFailure("the facade has no switchRawProbeForms: (a release plug-in)")
        }
        if let refusal = object.perform(selector, with: forms)?.takeUnretainedValue() as? String {
            throw TestFailure(refusal)
        }
    }
}
