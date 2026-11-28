import Foundation
import HWTestCore
import Metal

/// gpu.perf.nvn's zcull rule arms (zcull.h zcullInvalidates, switchMetalProbeZcullKeep:): whether a region stays right
/// when a draw writes depth under ALWAYS or NOT_EQUAL, which the driver invalidates the region for today.
/// - zcull_always, zcull_not_equal: a near draw at 0.2 under LESS, a full-screen draw at 0.8 writing its depth under
///   ALWAYS (NOT_EQUAL), then 16 heavy layers at 0.5 under LESS. Every layer must draw, as the depth they test is the
///   0.8 the second draw wrote. `_culled_px` counts pixels that still hold the second draw's colour. With the probe off
///   the rule invalidates the region (`_off_zcull_invalidations` 1 a run); on, it keeps it (`_on_zcull_rules_kept`).
///   0 culled pixels on means the region follows the depth the ROP writes, and the rule can go.
/// - zcull_rule_control: a triangle at 0.1 whose program writes 0.8, then the same layers. The probe keeps the region
///   past the fragment-depth rule, which is known to be needed, so its on arm must cull pixels.
///   `zcull_rules_conclusive` is 1 where it did; 0 means zcull culled nothing here, and the arms above say nothing.
extension GPURates {
    func zcullRules() async throws {
        let flat = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let writes = try gpu.render(fragment: "rates_set_depth_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let heavy = try gpu.render(fragment: "rates_heavy_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let colour = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let depth = try gpu.texture(.depth32Float, s.width, s.height, usage: [.renderTarget])
        let less = try depthState(.less)
        let near = SIMD4<Float>(1, 0, 0, 1), middle = SIMD4<Float>(0, 1, 0, 1)
        let culled = expected(middle)
        let layers = { (e: MTLRenderCommandEncoder) in
            e.setDepthStencilState(less)
            e.setRenderPipelineState(heavy)
            var n = self.s.heavyIterations
            e.setFragmentBytes(&n, length: 4, index: 0)
            for _ in 0 ..< self.s.layers { self.quad(e, z: 0.5) }
        }
        for (arm, compare, bit) in [
            ("zcull_always", MTLCompareFunction.always, Self.kZcullKeepAlways),
            ("zcull_not_equal", .notEqual, Self.kZcullKeepNotEqual),
        ] {
            let written = try depthState(compare)
            try await ab(
                arm, bits: bit, proof: "zcull_rules_kept", held: ["zcull_invalidations"], heldOff: false,
                read: colour, culled: culled, probeName: "switchMetalProbeZcullKeep:"
            ) { c in
                let e = self.pass(colour, depth, c)
                e.setDepthStencilState(less)
                e.setRenderPipelineState(flat)
                var k = near
                e.setFragmentBytes(&k, length: 16, index: 0)
                self.quad(e, z: 0.2)
                e.setDepthStencilState(written)
                var m = middle
                e.setFragmentBytes(&m, length: 16, index: 0)
                self.quad(e, z: 0.8)
                layers(e)
                e.endEncoding()
            }
        }
        try await ab(
            "zcull_rule_control", bits: Self.kZcullKeepFragmentDepth, proof: "zcull_rules_kept",
            held: ["zcull_invalidations"], heldOff: false, read: colour, culled: culled,
            probeName: "switchMetalProbeZcullKeep:"
        ) { c in
            let e = self.pass(colour, depth, c)
            e.setDepthStencilState(less)
            e.setRenderPipelineState(writes)
            var w = SIMD4<Float>(middle.x, middle.y, middle.z, 0.8)
            e.setFragmentBytes(&w, length: 16, index: 0)
            self.quad(e, z: 0.1)
            layers(e)
            e.endEncoding()
        }
        if case .integer(let n) = metrics["zcull_rule_control_on_culled_px"] {
            metrics.set("zcull_rules_conclusive", n > 0)
        }
    }

    static let kZcullKeepAlways: UInt32 = 1, kZcullKeepNotEqual: UInt32 = 2, kZcullKeepFragmentDepth: UInt32 = 4
}
