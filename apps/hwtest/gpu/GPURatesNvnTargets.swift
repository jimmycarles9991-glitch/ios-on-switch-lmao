import Foundation
import HWTestCore
import Metal

/// gpu.perf.nvn's render-target arms (GPURatesNvn.swift): discard, cbr and tiled_flush
extension GPURates {
    static let cbrKinds = ["bgra8": 0xe0, "rgba16f": 0xee]

    /// A target filled with one colour, then a pass that loads it, draws another over all of it and stores DontCare
    func discard() async throws {
        let solid = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let target = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let before = SIMD4<Float>(1, 1, 0, 1), drawn = SIMD4<Float>(0, 1, 1, 1)
        let fill = { (c: MTLCommandBuffer) in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 1, green: 1, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            c.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        let redraw = { (c: MTLCommandBuffer) in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .dontCare
            let e = c.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(solid)
            var k = drawn
            e.setFragmentBytes(&k, length: 16, index: 0)
            for _ in 0 ..< self.s.layers { self.quad(e, z: 0.5) }
            e.endEncoding()
        }
        let kept = expected(before)
        let probed = has("switchMetalProbeNvn:")
        for (state, on) in [("off", false), ("on", true)] {
            var best = Double.infinity
            let grew = try await grown(["nvn_discards"]) {
                for run in 0 ..< self.runs {
                    try await self.gpu.run(fill)
                    self.probe("switchMetalProbeNvn:", on ? Self.nvnDiscard : 0)
                    let t = try await self.gpu.run(redraw)
                    self.probe("switchMetalProbeNvn:", 0)
                    if run > 0 { best = min(best, t.gpuMilliseconds) }
                }
            }
            metrics.set("discard_\(state)_us", best * 1000, digits: 1)
            proved("discard_\(state)", grew, "nvn_discards", checked: on && probed)
            let got = try await gpu.read(target, bytesPerPixel: 4)
            metrics.set("discard_\(state)_kept_px", zip(got, kept).filter { $0 == $1 }.count / 4)
        }
    }

    /// 4x targets on 2CRA and on 2CBR: 16 layers of fill, resolved into a one-sample texture, the resolves compared
    func cbr() async throws {
        let probed = has("switchMetalProbeMs4Cbr:")
        for (name, format, bytes) in [("bgra8", MTLPixelFormat.bgra8Unorm, 4), ("rgba16f", .rgba16Float, 8)] {
            let solid = try gpu.render(fragment: "solid_fragment", format: format, samples: 4)
            var resolved: [[UInt8]] = []
            for (state, on) in [("off", false), ("on", true)] {
                let arm = "cbr_\(name)_\(state)"
                // held through the arm: a target made plain takes its compressed memory, and its kind, at a later pass
                probe("switchMetalProbeMs4Cbr:", on ? 1 : 0)
                defer { probe("switchMetalProbeMs4Cbr:", 0) }
                let ms = try await compressedTarget(format)
                let into = try gpu.texture(format, s.width, s.height)
                let before = facts(ms)
                let t = try await fastest(arm) { c in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = ms
                    pass.colorAttachments[0].resolveTexture = into
                    pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].storeAction = .multisampleResolve
                    let e = c.makeRenderCommandEncoder(descriptor: pass)!
                    e.setRenderPipelineState(solid)
                    for i in 0 ..< self.s.layers {
                        var k = SIMD4<Float>(Float(i) / Float(self.s.layers), 0.5, 0.25, 1)
                        e.setFragmentBytes(&k, length: 16, index: 0)
                        self.quad(e, z: 0.5)
                    }
                    e.endEncoding()
                }
                metrics.set("\(arm)_us", t * 1000, digits: 1)
                let after = facts(ms)
                let kind = after["kind"] ?? 0
                metrics.set("\(arm)_kind", "0x" + String(kind, radix: 16))
                metrics.set("\(arm)_comptags", after["comptags"] ?? 0)
                if probed && after["compression"] == 1 {
                    if before["comptags"] != 1 || after["comptags"] != 1 {
                        problems.append("\(arm): the target ran on plain memory for some of its runs")
                    } else if before["kind"] != kind {
                        problems.append("\(arm): the target's kind changed while it was timed")
                    } else if on && kind != Self.cbrKinds[name] {
                        problems.append("\(arm): the target's kind is 0x\(String(kind, radix: 16)), not 2CBR's")
                    }
                }
                resolved.append(try await gpu.read(into, bytesPerPixel: bytes))
            }
            metrics.set("cbr_\(name)_differ_bytes", zip(resolved[0], resolved[1]).filter { $0 != $1 }.count)
        }
    }

    /// A 4x target on compressed memory where the facade can bind some: the first of a size wants memory the compression
    /// thread then makes, so where it got none, a second is made once the thread settled (HWTestKit
    /// CompressionSettled.remade)
    func compressedTarget(_ format: MTLPixelFormat) async throws -> MTLTexture {
        let make = { try self.gpu.texture(format, self.s.width, self.s.height, usage: [.renderTarget], samples: 4) }
        let first = try make()
        guard facts(first)["comptags"] == 0, let settleCompression else { return first }
        try await settleCompression()
        return try make()
    }

    /// Pass 1 draws 16 heavy layers into a target; pass 2, in the same command buffer, copies it into another
    func tiledFlush() async throws {
        let heavy = try gpu.render(fragment: "rates_heavy_fragment", format: .bgra8Unorm)
        let copy = try gpu.render(fragment: "rates_copy_fragment", format: .bgra8Unorm)
        let first = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let second = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let encode = { (c: MTLCommandBuffer) in
            let e = self.pass(first, nil, c)
            e.setRenderPipelineState(heavy)
            var n = self.s.heavyIterations
            e.setFragmentBytes(&n, length: 4, index: 0)
            for _ in 0 ..< self.s.layers { self.quad(e, z: 0.5) }
            e.endEncoding()
            let f = self.pass(second, nil, c)
            f.setRenderPipelineState(copy)
            f.setFragmentTexture(first, index: 0)
            self.quad(f, z: 0.5)
            f.endEncoding()
        }
        try await gpu.run(encode)
        let today = try await gpu.read(second, bytesPerPixel: 4)
        let probed = (gpu.queue.makeCommandBuffer() as? NSObject)?
            .responds(to: NSSelectorFromString("switchMetalProbeForms:")) ?? false
        let tiledCache = facts(first)["tiled_cache"] == 1
        let arms = [("off", "switch=nowfi pass=pixel"), ("on", "switch=nowfi_noflush pass=pixel")]
        for (state, forms) in arms {
            var last: MTLCommandBuffer?
            let t = try await fastest("tiled_flush_\(state)") { c in
                if probed { try self.forms(c, forms) }
                last = c
                encode(c)
            }
            metrics.set("tiled_flush_\(state)_us", t * 1000, digits: 1)
            let flushes = last.map { taken($0)["flushes"] ?? 0 } ?? 0
            metrics.set("tiled_flush_\(state)_flushes", flushes)
            if probed && tiledCache && state == "on" && flushes == 0 {
                problems.append("tiled_flush_on: no pass ended without its flush (switchMetalProbeTaken flushes=0)")
            }
            let got = try await gpu.read(second, bytesPerPixel: 4)
            metrics.set("tiled_flush_\(state)_wrong", zip(got, today).filter { $0 != $1 }.count / 4)
        }
    }
}
