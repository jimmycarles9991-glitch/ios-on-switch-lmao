import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Rule fetch.blend (drivers/TegraGM20BMetal/raw/runtime/raw_fetch_rungs.h fixedBlendAcceptable) on the chip: N
/// premultiplied source-over layers into one BGRA8 target, drawn once through the ROP's blend and once by a program that
/// fetches the destination and blends it (the fetch rung the rule would replace), each against Metal's exact blend
/// computed here (every layer stored as the nearest code). Sources are 8-bit codes or codes plus sixteenths (a
/// shader-computed colour), from shaders/FixedBlendRule.metal's hash. The rule's claim: the ROP stays within 2 codes for
/// 8-bit sources at any depth and for computed sources up to 2 layers; 8 computed layers are reported (x1-units' model
/// says 4). Then 8 layers over 1280x720 both ways give each route's GPU time.
struct FixedBlendRuleTest: HWTest {
    let id = "gpu.raw.fixed_blend_rule"
    let name = "Fixed-function blend against the fetch rung: codes within the rule's bounds, GPU time both ways"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let size = 128
    static let clear: [Double] = [0.2, 0.4, 0.6, 1.0]

    static func hash(_ x: UInt32, _ y: UInt32, _ layer: UInt32) -> UInt32 {
        var h = (x &* 73_856_093) ^ (y &* 19_349_663) ^ (layer &* 83_492_791)
        h ^= h >> 13
        h = h &* 0x5bd1_e995
        h ^= h >> 15
        return h
    }

    /// The layer's premultiplied RGBA at (x, y), as the shader computes it
    static func source(_ x: Int, _ y: Int, layer: Int, computed: Bool) -> [Double] {
        let h = hash(UInt32(x), UInt32(y), UInt32(layer))
        let alpha = h & 255
        let codes = [h >> 8, h >> 16, h >> 24].map { ($0 & 255) * alpha / 255 }
        let a = Double(Float(alpha) * (Float(1) / Float(255)))
        guard computed else { return codes.map { Double(Float($0) * (Float(1) / Float(255))) } + [a] }
        let frac = [h >> 4, h >> 12, h >> 20].map { ($0 & 15) * min(alpha, 1) }
        return zip(codes, frac).map { Double(Float($0 * 16 + $1) * (Float(1) / Float(4080))) } + [a]
    }

    /// Metal's result of `layers` source-overs at (x, y), RGBA codes
    static func reference(_ x: Int, _ y: Int, layers: Int, computed: Bool) -> [Int] {
        var d = clear.map { ($0 * 255).rounded() }
        for layer in 0..<layers {
            let s = source(x, y, layer: layer, computed: computed)
            d = (0..<4).map { c in (min(max(s[c] + d[c] / 255 * (1 - s[3]), 0), 1) * 255).rounded() }
        }
        return d.map { Int($0) }
    }

    struct Errors {
        var worst = 0, off = 0, sum = 0, channels = 0
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let rop = try gpu.render(fragment: "rule_rop_fragment", format: .bgra8Unorm) { d in
            let a = d.colorAttachments[0]!
            a.isBlendingEnabled = true
            a.sourceRGBBlendFactor = .one
            a.sourceAlphaBlendFactor = .one
            a.destinationRGBBlendFactor = .oneMinusSourceAlpha
            a.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        }
        let fetch = try gpu.render(fragment: "rule_fetch_fragment", format: .bgra8Unorm)
        var metrics = Metrics()
        var wrong: [String] = []
        var paths: [String] = []
        for computed in [false, true] {
            for layers in [1, 2, 8] {
                for (route, state) in [("rop", rop), ("fetch", fetch)] {
                    let target = try gpu.texture(.bgra8Unorm, Self.size, Self.size)
                    paths.append(try await gpu.run(Self.layers(state, target, count: layers, computed: computed)).path)
                    let bytes = try await gpu.read(target, bytesPerPixel: 4)
                    let e = Self.errors(bytes, layers: layers, computed: computed)
                    let label = "\(route)_\(computed ? "computed" : "8bit")_\(layers)"
                    metrics.set("\(label)_worst", e.worst)
                    metrics.set("\(label)_off_pct", Double(e.off) * 100 / Double(e.channels), digits: 2)
                    metrics.set("\(label)_mean", Double(e.sum) / Double(e.channels), digits: 3)
                    if route == "rop" && (!computed || layers <= 2) && e.worst > 2 { wrong.append(label) }
                }
            }
        }
        let big = try gpu.texture(.bgra8Unorm, 1280, 720)
        var ropMs = 0.0, fetchMs = 0.0
        for _ in 0..<3 {
            ropMs += try await gpu.run(Self.layers(rop, big, count: 8, computed: true)).gpuMilliseconds
            fetchMs += try await gpu.run(Self.layers(fetch, big, count: 8, computed: true)).gpuMilliseconds
        }
        metrics.set("rop_gpu_ms_8_layers_720p", ropMs / 3, digits: 3)
        metrics.set("fetch_gpu_ms_8_layers_720p", fetchMs / 3, digits: 3)
        metrics.set("paths", paths.allSatisfy { $0 == "raw" } ? "raw" : paths.joined(separator: ","))
        if paths.contains(where: { $0 != "raw" }) { wrong.append("paths") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    static func layers(
        _ state: MTLRenderPipelineState, _ target: MTLTexture, count: Int, computed: Bool
    )
        -> (MTLCommandBuffer) -> Void
    {
        { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(
                red: clear[0], green: clear[1], blue: clear[2], alpha: clear[3])
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            for layer in 0..<count {
                var key = SIMD2<UInt32>(UInt32(layer), computed ? 1 : 0)
                encoder.setFragmentBytes(&key, length: MemoryLayout<SIMD2<UInt32>>.stride, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
    }

    static func errors(_ bytes: [UInt8], layers: Int, computed: Bool) -> Errors {
        var e = Errors()
        for y in 0..<size {
            for x in 0..<size {
                let want = reference(x, y, layers: layers, computed: computed)
                let texel = Array(bytes.pixel(x, y, width: size))
                let got = [Int(texel[2]), Int(texel[1]), Int(texel[0]), Int(texel[3])]
                for c in 0..<4 {
                    let d = abs(got[c] - want[c])
                    e.worst = max(e.worst, d)
                    e.off += d > 0 ? 1 : 0
                    e.sum += d
                    e.channels += 1
                }
            }
        }
        return e
    }
}
