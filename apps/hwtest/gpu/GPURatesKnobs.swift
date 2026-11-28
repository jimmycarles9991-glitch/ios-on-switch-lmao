import Foundation
import HWTestCore
import Metal

/// gpu.perf.knobs: two fixed-function settings A/B'd through the facade's probes.
///
/// - SET_SUBTILING_PERF_KNOB_A: our 0x20164010 (NVK's desktop value, GM20B's golden one) against uam's 0x087F6080 and
///   0x20806080 (3 or more colour outputs); its commented-out 0x06164010 is gpu.perf.first_touch's. Each runs on a
///   fragment-heavy draw into one
///   target, into four, with a program-written depth, and on a plain fill. The `_us_768` fields scale a time by
///   `lane_mhz` / 768, which holds for these clock-bound draws. `subtile_<knob>_writes_per_pass` is 1 where every pass
///   wrote the knob. The channel keeps the last knob a pass wrote, so a pass of its own writes the golden one back after
///   each knob's arms.
/// - SECTOR_PROMOTION of block-linear texture headers: none, to 2V (deko3d's), to 2H and to 4, on a 2048x2048 RGBA8
///   texture sampled minified, sampled at 1:1, and read at scattered texels. These are bound by DRAM traffic, not the
///   clock, so they keep their measured times. `promo_<mode>_header` is the field the image's header held after its
///   arms (TIC bits 156:155).
///
/// An arm that throws records `<arm>_error` and the arms after it still run. uam's commented-out 0x06164010 has never run
/// on the chip: it is gpu.perf.first_touch's (GPURatesFirstTouch.swift), which runs after every other test.
extension GPURates {
    static let knobValues: [UInt32] = [0x2016_4010, 0x087F_6080, 0x2080_6080]
    static let lastKnob: UInt32 = 0x0616_4010
    static let golden: UInt32 = 0x2016_4010

    struct SubtileKit {
        let cases: [(String, MTLRenderPipelineState, Int, Bool)]
        let solid: MTLRenderPipelineState
        let always: MTLDepthStencilState
        let targets: [MTLTexture]
        let zeta: MTLTexture
    }

    public static func knobs(_ gpu: GPU, _ s: Sizes = .device) async throws -> SceneResult {
        let b = GPURates(gpu, s)
        try await b.alu()
        let kit = try b.subtileKit()
        var notes: [String] = []
        let knobbed = b.has("switchMetalProbeSubtilingKnob:")
        if knobbed {
            for knob in knobValues { await b.knobArms(knob, kit) }
        } else {
            notes.append("no subtiling arms beside the default: the device has no switchMetalProbeSubtilingKnob: probe")
            await b.isolated("subtile_default") { try await b.subtiling(nil, kit) }
        }
        let promotions: [UInt32] = b.has("switchMetalProbeSectorPromotion:") ? [0, 1, 2, 3] : [0]
        if promotions.count == 1 {
            notes.append("no promotion arms beside none: the device has no switchMetalProbeSectorPromotion: probe")
        }
        await b.isolated("promo") { try await b.promotion(promotions) }
        var r = SceneResult()
        r.metrics = b.metrics
        r.notes = notes
        for p in b.problems { r.expect(false, p) }
        return r
    }

    func subtileKit() throws -> SubtileKit {
        let one = try gpu.render(fragment: "rates_heavy_fragment", format: .bgra8Unorm)
        let solid = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let four = try gpu.render(fragment: "rates_heavy_mrt_fragment", format: .bgra8Unorm) { d in
            for j in 1 ..< 4 { d.colorAttachments[j].pixelFormat = .bgra8Unorm }
        }
        let depth = try gpu.render(fragment: "rates_heavy_depth_fragment", format: .bgra8Unorm, depth: .depth32Float)
        return SubtileKit(
            cases: [
                ("one", one, 1, false), ("four", four, 4, false), ("depth", depth, 1, true), ("fill", solid, 1, false),
            ],
            solid: solid, always: try depthState(.always),
            targets: try (0 ..< 4).map { _ in try gpu.texture(.bgra8Unorm, s.width, s.height) },
            zeta: try gpu.texture(.depth32Float, s.width, s.height, usage: [.renderTarget])
        )
    }

    func knobArms(_ knob: UInt32, _ kit: SubtileKit) async {
        await isolated("subtile_\(String(knob, radix: 16))") { try await subtiling(knob, kit) }
        await isolated("subtile_golden_back") {
            probe("switchMetalProbeSubtilingKnob:", Self.golden)
            defer { probe("switchMetalProbeSubtilingKnob:", 0) }
            try await gpu.run { c in
                let e = self.pass(kit.targets[0], nil, c)
                e.setRenderPipelineState(kit.solid)
                var colour = SIMD4<Float>(0, 0, 0, 1)
                e.setFragmentBytes(&colour, length: 16, index: 0)
                self.drawFullScreen(e, count: 1)
                e.endEncoding()
            }
        }
    }

    func drawFullScreen(_ e: MTLRenderCommandEncoder, count: Int) {
        var corners = GPU.fullScreen
        e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        for _ in 0 ..< count { e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4) }
    }

    /// The four draws under `knob` (nil: whatever the channel holds, for a device without the probe)
    func subtiling(_ knob: UInt32?, _ kit: SubtileKit) async throws {
        if let knob { probe("switchMetalProbeSubtilingKnob:", knob) }
        let tag = knob.map { String($0, radix: 16) } ?? "default"
        let grew = try await grown(["subtile_writes"]) {
            for (name, pipeline, colours, writesDepth) in kit.cases {
                let t = try await fastest("subtile_\(name)_\(tag)") { c in
                    let pass = MTLRenderPassDescriptor()
                    for j in 0 ..< colours {
                        pass.colorAttachments[j].texture = kit.targets[j]
                        pass.colorAttachments[j].loadAction = .clear
                        pass.colorAttachments[j].storeAction = .store
                    }
                    if writesDepth {
                        pass.depthAttachment.texture = kit.zeta
                        pass.depthAttachment.loadAction = .clear
                        pass.depthAttachment.storeAction = .dontCare
                    }
                    let e = c.makeRenderCommandEncoder(descriptor: pass)!
                    e.setRenderPipelineState(pipeline)
                    if writesDepth { e.setDepthStencilState(kit.always) }
                    var n = self.s.heavyIterations
                    var colour = SIMD4<Float>(0.2, 0.4, 0.6, 1)
                    if pipeline === kit.solid {
                        e.setFragmentBytes(&colour, length: 16, index: 0)
                    } else {
                        e.setFragmentBytes(&n, length: 4, index: 0)
                    }
                    self.drawFullScreen(e, count: self.s.layers)
                    e.endEncoding()
                }
                metrics.set("subtile_\(name)_\(tag)_us", t * 1000, digits: 1)
                if laneMhz > 0 { metrics.set("subtile_\(name)_\(tag)_us_768", t * 1000 * laneMhz / 768, digits: 1) }
            }
        }
        guard knob != nil else { return }
        let passes = Double((1 + s.repeats) * kit.cases.count)
        let writes = grew["subtile_writes"] ?? 0
        metrics.set("subtile_\(tag)_writes_per_pass", Double(writes) / passes, digits: 2)
        if writes <= 0 { problems.append("subtile_\(tag): no pass wrote the knob (subtile_writes did not grow)") }
    }

    /// A 2048x2048 texture per mode, made under it on plain memory and copied from one source, sampled three ways. The
    /// source stays unpromoted; only `image`'s header is under the mode.
    func promotion(_ modes: [UInt32]) async throws {
        let side = 2048
        let source = try gpu.texture(.rgba8Unorm, side, side, usage: [.shaderRead, .renderTarget])
        let bytes = (0 ..< side * side * 4).map { UInt8(truncatingIfNeeded: ($0 &* 40503) >> 9) }
        try await gpu.upload(bytes, to: source, bytesPerRow: side * 4)
        let sample = try gpu.render(fragment: "rates_sample_fragment", format: .bgra8Unorm)
        let scatter = try gpu.render(fragment: "rates_scatter_fragment", format: .bgra8Unorm)
        let target = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let probed = has("switchMetalProbeSectorPromotion:")
        for mode in modes {
            // held through the mode's arms: a header written later takes it too
            probe("switchMetalProbeSectorPromotion:", mode)
            defer { probe("switchMetalProbeSectorPromotion:", 0) }
            // plain in every mode, so no mode's image is moved to compressed memory and read through its comptags
            probe("switchMetalProbePlainPrivate:", 1)
            let made = Result { try gpu.texture(.rgba8Unorm, side, side, usage: [.shaderRead, .renderTarget]) }
            probe("switchMetalProbePlainPrivate:", 0)
            let image = try made.get()
            try await gpu.run { c in
                let e = c.makeBlitCommandEncoder()!
                e.copy(from: source, to: image)
                e.endEncoding()
            }
            let cases: [(String, MTLRenderPipelineState, Float)] = [
                ("minified", sample, 1.6 / Float(s.width)), ("one", sample, 1 / Float(side)), ("scatter", scatter, 0),
            ]
            for (name, pipeline, scale) in cases {
                let t = try await fastest("promo_\(name)_\(mode)") { c in
                    let e = self.pass(target, nil, c)
                    e.setRenderPipelineState(pipeline)
                    e.setFragmentTexture(image, index: 0)
                    var step = scale
                    e.setFragmentBytes(&step, length: 4, index: 0)
                    self.drawFullScreen(e, count: self.s.layers)
                    e.endEncoding()
                }
                metrics.set("promo_\(name)_\(mode)_us", t * 1000, digits: 1)
            }
            guard let word = headerWord(image, 4) else { continue }
            let field = (word >> 27) & 3
            metrics.set("promo_\(mode)_header", Int(field))
            if probed && field != mode { problems.append("promo_\(mode): the image's header holds promotion \(field)") }
        }
    }
}
