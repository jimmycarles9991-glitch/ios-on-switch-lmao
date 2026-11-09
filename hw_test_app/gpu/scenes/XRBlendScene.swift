import Foundation
import HWTestCore
import Metal

/// The blender's rounding on a 10-bit unorm target under ONE / ONE_MINUS_SOURCE1_ALPHA, the blend that composites
/// Core Animation's src-over into BGR10_XR through an A2B10G10R10 view. Column x holds code x in red, green and blue;
/// row r draws S (a quarter-code offset per channel, so ties and near-ties come up) with source1 alpha a. Each channel
/// is checked against RNE(1023 S + x (1 - a)) clamped to [0, 1023], from the fp32 values the fragment returns.
/// rne=pass: the device rounds the exact value to nearest even; within1=pass: every code is at most one off (the
/// bound the BGR10_XR blend path is approved under, and the scene's pass condition); trunc= and up= count matches with
/// truncation and with ties up; err_m1/err_0/err_p1 the signed error histogram.
struct XRBlendScene: GPUScene {
    let id = "gpu.xr_blend"
    let name = "A2B10G10R10 blend rounding under source1 alpha"

    static let width = 1024
    static let rows = 1024

    /// Row r: S codes for red, green and blue (base + 0, + 0.25, + 0.5) and source1 alpha
    static func row(_ r: Int) -> SIMD4<Float> {
        let base = Float((r * 389) % 1024)
        let alpha: Float = r < 256 ? Float(r) / 255 : r < 512 ? 1 - Float(r - 255) / 65536 : Float(r % 509) / 509
        return SIMD4(base / 1023, (base + 0.25) / 1023, (base + 0.5) / 1023, alpha)
    }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let (width, height) = (Self.width, Self.rows)
        let target = try gpu.texture(.rgb10a2Unorm, width, height, usage: [.renderTarget, .shaderRead])
        var fill: [UInt8] = []
        fill.reserveCapacity(width * height * 4)
        for _ in 0..<height {
            for x in 0..<width {
                let word = UInt32(x) | UInt32(x) << 10 | UInt32(x) << 20 | 3 << 30
                fill += withUnsafeBytes(of: word.littleEndian, Array.init)
            }
        }
        try await gpu.blitUpload(
            [UploadRect(x: 0, y: 0, width: width, height: height)], [fill], into: target, texel: 4)
        let rows = (0..<height).map(Self.row)
        let rowBuffer = try gpu.buffer(rows)
        let pipeline = try gpu.render(fragment: "xr_blend_fragment", format: .rgb10a2Unorm) { descriptor in
            let attachment = descriptor.colorAttachments[0]!
            attachment.isBlendingEnabled = true
            attachment.sourceRGBBlendFactor = .one
            attachment.destinationRGBBlendFactor = .oneMinusSource1Alpha
            attachment.sourceAlphaBlendFactor = .one
            attachment.destinationAlphaBlendFactor = .zero
        }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBuffer(rowBuffer, offset: 0, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        let bytes = try await gpu.read(target, bytesPerPixel: 4)
        var counts = (rne: 0, trunc: 0, up: 0, alpha: 0)
        var histogram = [Int: Int]()
        var first: String?
        for y in 0..<height {
            let row = rows[y]
            let a = Double(row[3])
            for x in 0..<width {
                let at = 4 * (y * width + x)
                let word =
                    UInt32(bytes[at]) | UInt32(bytes[at + 1]) << 8 | UInt32(bytes[at + 2]) << 16
                    | UInt32(bytes[at + 3]) << 24
                counts.alpha += word >> 30 == 3 ? 0 : 1
                for c in 0..<3 {
                    let exact = 1023 * Double(row[c]) + Double(x) * (1 - a)
                    let clamp = { (v: Double) in Int(min(max(v, 0), 1023)) }
                    let want = clamp(exact.rounded(.toNearestOrEven)), have = Int(word >> (10 * UInt32(c)) & 0x3FF)
                    counts.trunc += have == clamp(exact.rounded(.down)) ? 1 : 0
                    counts.up += have == clamp(exact.rounded(.toNearestOrAwayFromZero)) ? 1 : 0
                    histogram[have - want, default: 0] += 1
                    if have == want {
                        counts.rne += 1
                    } else if first == nil {
                        first = "x \(x) row \(y) c \(c) S \(row[c]) a \(row[3]): \(have), want \(want) (\(exact))"
                    }
                }
            }
        }
        let total = width * height * 3
        let worst = histogram.keys.map(abs).max() ?? 0
        result.metrics.set("channels", total)
        result.metrics.set("rne", counts.rne == total ? "pass" : "fail")
        result.metrics.set("rne_matches", counts.rne)
        result.metrics.set("trunc_matches", counts.trunc)
        result.metrics.set("up_matches", counts.up)
        result.metrics.set("within1", worst <= 1 ? "pass" : "fail")
        result.metrics.set("max_error", worst)
        for e in -1...1 { result.metrics.set("err_\(e < 0 ? "m1" : e == 0 ? "0" : "p1")", histogram[e, default: 0]) }
        if let first { result.metrics.set("rne_first", first) }
        result.expect(worst <= 1, "a blended code \(worst) from RNE of the exact value (\(first ?? ""))")
        result.expect(counts.alpha == 0, "\(counts.alpha) texels lost alpha 3 under ONE / ZERO")
        return result
    }
}
