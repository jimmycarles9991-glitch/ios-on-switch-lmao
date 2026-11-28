import Foundation
import HWTestCore
import Metal

/// gpu.perf.nvn: state NVN writes and the driver does not, A/B'd through the facade's probes (nvn_probes.h,
/// learn/deko3d.md). Each arm times the same work with the probe off (`_off`) and on (`_on`) and reports, per run, how
/// far the counters its probe moves grew (`_<counter>`). An on arm whose probe exists and moved nothing is a problem:
/// its time would be the off arm's. Where the state could change pixels, it also counts the pixels that differ from the
/// expected ones (`_wrong`). An arm that throws records `<arm>_error`, every probe goes off and the next arm runs. The
/// arms that push state this driver never pushed on the chip before (Z_4X2, discard) are gpu.perf.first_touch's
/// (GPURatesFirstTouch.swift), which runs after every other test.
/// - zcull_bounds: a program writes a depth farther than its triangle's, then 16 layers of a 64-FMA program sit between
///   the two depths and must be drawn. A second form has the written depth nearer, which hides the layers. Off, the
///   program invalidates the region (`_zcull_invalidations`); on, it opens zcull's bounds instead
///   (`_nvn_bounds_opened`).
/// - stencil_zcull: 16 layers whose stencil test fails everywhere, under SET_ZCULL with stencil on and NVN's NOTEQUAL
///   criterion, the criterion set for the on arm only.
/// - zcull_fp: the hidden-layer pass of gpu.perf.rates under ZFORMAT FP for its clear of 1.0.
/// - cbr: a 4x BGRA8 and a 4x RGBA16F target on NVN's 2CBR kinds against 2CRA, filled and resolved, the probe held
///   through the arm. The resolved pixels must be equal; `_kind` and `_comptags` are the target's.
/// - tiled_flush: a pass whose target the next pass reads, both arms under pass=pixel switch=nowfi, the on arm without
///   the tiled-cache flush at a pass end (switch=nowfi_noflush). `_flushes` counts the passes that ended without it;
///   `_wrong` counts pixels against today's barriers.
/// - zcull_always, zcull_not_equal, zcull_rule_control: GPURatesZcullRules.swift.
extension GPURates {
    public static func nvn(
        _ gpu: GPU, _ s: Sizes = .device, settleCompression: (() async throws -> Void)? = nil
    ) async throws -> SceneResult {
        let b = GPURates(gpu, s)
        b.settleCompression = settleCompression
        var notes: [String] = []
        if !b.has("switchMetalProbeNvn:") {
            notes.append("the device has no switchMetalProbeNvn: probe: each arm's on form is its off form")
        }
        await b.isolated("zcull_bounds") { try await b.zcullBounds() }
        await b.isolated("stencil_zcull") { try await b.stencilZcull() }
        await b.isolated("zcull_fp") { try await b.zcullFormat("zcull_fp", Self.nvnZcullFp) }
        await b.isolated("cbr") { try await b.cbr() }
        await b.isolated("tiled_flush") { try await b.tiledFlush() }
        await b.isolated("zcull_rules") { try await b.zcullRules() }
        var r = SceneResult()
        r.metrics = b.metrics
        r.notes = notes
        for p in b.problems { r.expect(false, p) }
        return r
    }

    static let nvnZcullBounds: UInt32 = 1, nvnStencilZcull: UInt32 = 2, nvnZcull4x2: UInt32 = 4
    static let nvnZcullFp: UInt32 = 8, nvnDiscard: UInt32 = 16
    static let notEqualCriterion: UInt32 = 5 | 0xff << 16

    var runs: Int { 1 + s.repeats }

    /// `encode` timed with the probe off and on, each its fastest run; the target read back after the last. `held`:
    /// counters that must not grow with the probe on, and with it off too where `heldOff`
    func ab(
        _ arm: String, bits: UInt32, proof: String, held: [String] = [], heldOff: Bool = true, criterion: UInt32 = 0,
        read: MTLTexture? = nil, want: [UInt8]? = nil, culled: [UInt8]? = nil,
        probeName: String = "switchMetalProbeNvn:", _ encode: (MTLCommandBuffer) throws -> Void
    ) async throws {
        let probed = has(probeName)
        for (state, on) in [("off", false), ("on", true)] {
            probe(probeName, on ? bits : 0)
            if criterion != 0 { probe("switchMetalProbeZcullCriterion:", on ? criterion : 0) }
            var t = 0.0
            let grew = try await grown([proof] + held) { t = try await fastest("\(arm)_\(state)", encode) }
            probe(probeName, 0)
            probe("switchMetalProbeZcullCriterion:", 0)
            metrics.set("\(arm)_\(state)_us", t * 1000, digits: 1)
            proved("\(arm)_\(state)", grew, proof, checked: on && probed)
            for key in held where probed && (on || heldOff) && (grew[key] ?? 0) > 0 {
                problems.append("\(arm)_\(state): \(key) grew, so zcull was off for some of its draws")
            }
            if let read, want != nil || culled != nil {
                let got = try await gpu.read(read, bytesPerPixel: 4)
                if let want { metrics.set("\(arm)_\(state)_wrong", zip(got, want).filter { $0 != $1 }.count / 4) }
                if let culled { metrics.set("\(arm)_\(state)_culled_px", Self.pixelsEqual(got, culled)) }
            }
        }
    }

    /// Each counter's growth per run as `<arm>_<counter>`; a problem where `checked` and `proof` did not grow
    func proved(_ arm: String, _ grew: [String: Int], _ proof: String, checked: Bool) {
        for (key, n) in grew { metrics.set("\(arm)_\(key)", Double(n) / Double(runs), digits: 2) }
        if checked && (grew[proof] ?? 0) <= 0 {
            problems.append("\(arm): its probe never fired (\(proof) did not grow)")
        }
    }

    /// The pixels of `got` (BGRA8) equal to `want`'s
    static func pixelsEqual(_ got: [UInt8], _ want: [UInt8]) -> Int {
        stride(from: 0, to: min(got.count, want.count), by: 4).filter { i in
            got[i] == want[i] && got[i + 1] == want[i + 1] && got[i + 2] == want[i + 2] && got[i + 3] == want[i + 3]
        }.count
    }

    func expected(_ rgba: SIMD4<Float>) -> [UInt8] {
        let bgra = [rgba.z, rgba.y, rgba.x, rgba.w].map { UInt8(($0 * 255).rounded()) }
        return [UInt8]([[UInt8]](repeating: bgra, count: s.width * s.height).joined())
    }

    func quad(_ e: MTLRenderCommandEncoder, z: Float) {
        var corners = GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: z)
        e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    }

    /// The depth-writing program's draw, its triangle at 0.1, then the heavy layers at 0.3: drawn where it wrote 0.6
    /// (`seen`), hidden where it wrote 0.2
    func zcullBounds() async throws {
        let writes = try gpu.render(fragment: "rates_set_depth_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let heavy = try gpu.render(fragment: "rates_heavy_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let colour = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let depth = try gpu.texture(.depth32Float, s.width, s.height, usage: [.renderTarget])
        let less = try depthState(.less)
        for (form, written) in [("seen", Float(0.6)), ("hidden", Float(0.2))] {
            let behind = SIMD4<Float>(0, 0, 1, written)
            var want: [UInt8]?
            if form == "hidden" { want = expected(SIMD4(0, 0, 1, 1)) }
            let checked = want == nil ? nil : colour
            try await ab(
                "zcull_bounds_\(form)", bits: Self.nvnZcullBounds, proof: "nvn_bounds_opened",
                held: ["zcull_invalidations"], heldOff: false, read: checked, want: want
            ) { c in
                let e = self.pass(colour, depth, c)
                e.setDepthStencilState(less)
                e.setRenderPipelineState(writes)
                var w = behind
                e.setFragmentBytes(&w, length: 16, index: 0)
                self.quad(e, z: 0.1)
                e.setRenderPipelineState(heavy)
                var n = self.s.heavyIterations
                e.setFragmentBytes(&n, length: 4, index: 0)
                for _ in 0 ..< self.s.layers { self.quad(e, z: 0.3) }
                e.endEncoding()
            }
            if form == "seen" {
                let got = try await gpu.read(colour, bytesPerPixel: 4)
                let still = expected(SIMD4(0, 0, 1, 1))
                metrics.set("zcull_bounds_seen_culled_px", Self.pixelsEqual(got, still))
            }
        }
    }

    /// A stencil of 1 everywhere, then 16 heavy layers whose stencil test (not equal to 1) fails everywhere
    func stencilZcull() async throws {
        let format = MTLPixelFormat.depth32Float_stencil8
        let flat = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm, depth: format, stencil: format)
        let heavy = try gpu.render(
            fragment: "rates_heavy_fragment",
            format: .bgra8Unorm,
            depth: format,
            stencil: format
        )
        let colour = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let zeta = try gpu.texture(format, s.width, s.height, usage: [.renderTarget])
        let mark = MTLDepthStencilDescriptor(), test = MTLDepthStencilDescriptor()
        mark.depthCompareFunction = .always
        mark.frontFaceStencil.depthStencilPassOperation = .replace
        mark.backFaceStencil = mark.frontFaceStencil
        test.depthCompareFunction = .always
        test.frontFaceStencil.stencilCompareFunction = .notEqual
        test.backFaceStencil = test.frontFaceStencil
        guard let marks = gpu.device.makeDepthStencilState(descriptor: mark),
              let tests = gpu.device.makeDepthStencilState(descriptor: test)
        else { throw TestFailure("makeDepthStencilState returned nil") }
        let flatColour = SIMD4<Float>(0, 1, 0, 1)
        try await ab(
            "stencil_zcull", bits: Self.nvnStencilZcull, proof: "nvn_stencil_passes", held: ["zcull_invalidations"],
            criterion: Self.notEqualCriterion,
            read: colour, want: expected(flatColour)
        ) { c in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = zeta
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.storeAction = .dontCare
            pass.stencilAttachment.texture = zeta
            pass.stencilAttachment.loadAction = .clear
            pass.stencilAttachment.storeAction = .dontCare
            let e = c.makeRenderCommandEncoder(descriptor: pass)!
            e.setStencilReferenceValue(1)
            e.setDepthStencilState(marks)
            e.setRenderPipelineState(flat)
            var k = flatColour
            e.setFragmentBytes(&k, length: 16, index: 0)
            self.quad(e, z: 0.5)
            e.setDepthStencilState(tests)
            e.setRenderPipelineState(heavy)
            var n = self.s.heavyIterations
            e.setFragmentBytes(&n, length: 4, index: 0)
            for _ in 0 ..< self.s.layers { self.quad(e, z: 0.5) }
            e.endEncoding()
        }
    }

    /// gpu.perf.rates' hidden layers under one zcull format probe
    func zcullFormat(_ arm: String, _ bits: UInt32) async throws {
        let heavy = try gpu.render(fragment: "rates_heavy_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let flat = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let colour = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let depth = try gpu.texture(.depth32Float, s.width, s.height, usage: [.renderTarget])
        let lequal = try depthState(.lessEqual)
        let front = SIMD4<Float>(1, 0, 0, 1)
        try await ab(
            arm, bits: bits, proof: "nvn_format_passes", held: ["zcull_invalidations"], read: colour,
            want: expected(front)
        ) { c in
            let e = self.pass(colour, depth, c)
            e.setDepthStencilState(lequal)
            e.setRenderPipelineState(flat)
            var k = front
            e.setFragmentBytes(&k, length: 16, index: 0)
            self.quad(e, z: 0.1)
            e.setRenderPipelineState(heavy)
            var n = self.s.heavyIterations
            e.setFragmentBytes(&n, length: 4, index: 0)
            for _ in 0 ..< self.s.layers { self.quad(e, z: 0.5) }
            e.endEncoding()
        }
    }
}
