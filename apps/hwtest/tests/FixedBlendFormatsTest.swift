import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Rule fetch.blend's further formats (drivers/TegraGM20BMetal/fetch_rungs.h fixedBlendFormat): 8-bit
/// sources (gpu.gm20b.fixed_blend_rule's shader) blended through the ROP 1, 2 and 8 layers deep into RGBA16Float,
/// BGRA8Unorm_sRGB and RGBA8Unorm_sRGB, plus computed and HDR (> 1.0) arms into RGBA16Float, against Metal's blend
/// computed here (each layer stored as the format stores it, sRGB channels blended in linear light).
/// RGBA16Float is judged by value: the ROP takes the source in half precision, so where the source alpha is near 1 its
/// 1 - alpha moves a tiny result by several half steps (8 at the worst texel) while the value moves by under 2^-11.
/// Apple's GPU (M4 Max, the same shader and reference) is 1, 3 and 7 half steps and 2^-11, 2^-10 and 5 x 2^-11 off at
/// 1, 2 and 8 layers: the bound is 2^-11 a layer plus one. rgba16f_<n>_steps is the half steps, rgba16f_<n>_value the
/// largest value difference, rgba16f_<n>_model the half steps from a blend of the source rounded to half first (alpha
/// too), the model that puts the worst texel 8 steps off at every depth (reported, not judged). sRGB is judged at 1 and 2 layers, all the rule takes of 8-bit sources there; 8 layers are
/// reported (GM20B 3 codes, Apple's GPU 2). <arm>_<n>_at= is the worst texel: x,y,channel:got/want (half bits in hex,
/// codes in decimal).
struct FixedBlendFormatsTest: HWTest {
    let id = "gpu.gm20b.fixed_blend_formats"
    let name = "Fixed-function blend into RGBA16F and sRGB targets against Metal's blend"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let size = FixedBlendRuleTest.size
    static let hdrClear: [Double] = [1.5, 1.75, 1.25, 1.0]
    static let arms: [(label: String, format: MTLPixelFormat, computed: Bool, clear: [Double])] = [
        ("rgba16f", .rgba16Float, false, FixedBlendRuleTest.clear),
        ("rgba16f_computed", .rgba16Float, true, FixedBlendRuleTest.clear),
        ("rgba16f_hdr", .rgba16Float, true, hdrClear),
        ("bgra8_srgb", .bgra8Unorm_srgb, false, FixedBlendRuleTest.clear),
        ("rgba8_srgb", .rgba8Unorm_srgb, false, FixedBlendRuleTest.clear),
    ]

    struct Worst {
        var codes = 0, model = 0, value = 0.0
        var at = "-"
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        var wrong: [String] = []
        for arm in Self.arms {
            let state = try gpu.render(fragment: "rule_rop_fragment", format: arm.format) { d in
                let a = d.colorAttachments[0]!
                a.isBlendingEnabled = true
                a.sourceRGBBlendFactor = .one
                a.sourceAlphaBlendFactor = .one
                a.destinationRGBBlendFactor = .oneMinusSourceAlpha
                a.destinationAlphaBlendFactor = .oneMinusSourceAlpha
            }
            var paths: Set<String> = []
            for layers in [1, 2, 8] {
                let target = try gpu.texture(arm.format, Self.size, Self.size)
                let run = FixedBlendRuleTest.layers(
                    state, target, count: layers, computed: arm.computed, clear: arm.clear
                )
                paths.insert(try await gpu.run(run).path)
                let bytes = try await gpu.read(target, bytesPerPixel: arm.format == .rgba16Float ? 8 : 4)
                let worst = Self.worst(bytes, arm.format, layers: layers, computed: arm.computed, clear: arm.clear)
                let label = "\(arm.label)_\(layers)"
                metrics.set(label + "_at", worst.at)
                if arm.format == .rgba16Float {
                    metrics.set(label + "_steps", worst.codes)
                    metrics.set(label + "_model", worst.model)
                    metrics.set(label + "_value", worst.value, digits: 6)
                    let limit = Double(layers + 1) / (arm.clear[0] > 1 ? 1024 : 2048)
                    if worst.value > limit { wrong.append(label) }
                } else {
                    metrics.set(label + "_worst", worst.codes)
                    if layers <= 2 && worst.codes > 2 { wrong.append(label) }
                }
            }
            metrics.set(arm.label + "_path", paths.sorted().joined(separator: ","))
            if paths != ["gm20b"] { wrong.append(arm.label + "_path") }
        }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    /// The texel farthest from Metal's blend of `layers` source-overs into `format`: in codes (RGBA16F: half steps,
    /// negative zero read as zero) and, for RGBA16F, the largest value difference, which picks the texel
    static func worst(
        _ bytes: [UInt8], _ format: MTLPixelFormat, layers: Int, computed: Bool = false,
        clear: [Double] = FixedBlendRuleTest.clear
    ) -> Worst {
        typealias Rule = FixedBlendRuleTest
        let half = format == .rgba16Float
        var out = Worst()
        var farthest = -1.0
        for y in 0 ..< size {
            for x in 0 ..< size {
                var d = (0 ..< 4).map { c in Rule.store(clear[c], format, colour: c < 3) }
                var m = d
                for layer in 0 ..< layers {
                    let s = Rule.source(x, y, layer: layer, computed: computed)
                    d = (0 ..< 4).map { c in
                        Rule.store(s[c] + Rule.load(d[c], format, colour: c < 3) * (1 - s[3]), format, colour: c < 3)
                    }
                    let h = s.map { Double(Float(Float16(Float($0)))) }
                    m = (0 ..< 4).map { c in
                        Rule.store(h[c] + Rule.load(m[c], format, colour: c < 3) * (1 - h[3]), format, colour: c < 3)
                    }
                }
                let got = Rule.codes(bytes, x, y, format).map { half && $0 == 0x8000 ? 0 : $0 }
                for c in 0 ..< 4 {
                    let steps = abs(got[c] - d[c])
                    out.codes = max(out.codes, steps)
                    out.model = max(out.model, abs(got[c] - m[c]))
                    let distance =
                        half
                            ? abs(Rule.load(got[c], format, colour: false) - Rule.load(d[c], format, colour: false))
                            : Double(steps)
                    out.value = max(out.value, half ? distance : 0)
                    if distance > farthest {
                        farthest = distance
                        let shown = { (v: Int) in half ? String(v, radix: 16) : String(v) }
                        out.at = "\(x),\(y),\(c):\(shown(got[c]))/\(shown(d[c]))"
                    }
                }
            }
        }
        return out
    }
}
