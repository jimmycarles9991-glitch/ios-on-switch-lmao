import Foundation
import HWTestCore
import Metal

/// COMPILER.md E4: GM20B's ROP blend on BGRA8 against Metal's half arithmetic, per factor pair, over the domain Core
/// Animation feeds it: premultiplied s <= sa, each one of the 256 halves nearest k / 255 (32,896 pairs), over every
/// destination code. One 4096 x 2056 draw a pair: pixel p is pair p / 256 over code p % 256 (red and alpha; green and
/// blue over two other codes). The reference is the model an M-series Mac matches in every case (X1ProbeTests): each
/// product rounded to half, then their sum; a fused multiply-add misses it for dov, sat and xor.
public enum X1BlendProbes {
    static let width = 4096
    static let height = 2056

    public struct Pair: Sendable {
        public let name: String
        let source: (MTLBlendFactor, X1Reference.Factor)
        let destination: (MTLBlendFactor, X1Reference.Factor)
    }

    public static let pairs: [Pair] = [
        Pair(name: "sov", source: (.one, .one), destination: (.oneMinusSourceAlpha, .oneMinusSa)),
        Pair(name: "dov", source: (.oneMinusDestinationAlpha, .oneMinusDa), destination: (.one, .one)),
        Pair(name: "sin", source: (.destinationAlpha, .da), destination: (.zero, .zero)),
        Pair(name: "din", source: (.zero, .zero), destination: (.sourceAlpha, .sa)),
        Pair(name: "dot", source: (.zero, .zero), destination: (.oneMinusSourceAlpha, .oneMinusSa)),
        Pair(name: "sat", source: (.destinationAlpha, .da), destination: (.oneMinusSourceAlpha, .oneMinusSa)),
        Pair(
            name: "xor", source: (.oneMinusDestinationAlpha, .oneMinusDa),
            destination: (.oneMinusSourceAlpha, .oneMinusSa)),
        Pair(name: "add", source: (.one, .one), destination: (.one, .one)),
    ]

    static let halves: [Float16] = (0..<256).map { Float16(Double($0) / 255) }
    static let texels = (0..<256).map { X1Reference.texel($0, top: 255) }
    static let cases: [(UInt16, UInt16)] = (0..<256).flatMap { a in (0...a).map { (UInt16($0), UInt16(a)) } }

    /// The probe's target, its destination codes and the cases' buffers; one pair's draw (after the destination's
    /// copy) and its comparison
    public struct Rig {
        let gpu: GPU
        let staging: MTLBuffer, caseBuffer: MTLBuffer, halfBuffer: MTLBuffer
        let target: MTLTexture

        public init(_ gpu: GPU) throws {
            self.gpu = gpu
            var initial = [UInt8](repeating: 0, count: width * height * 4)
            for p in 0..<width * height {
                let (b, g, r, a) = destination(p)
                initial[p * 4] = UInt8(b)
                initial[p * 4 + 1] = UInt8(g)
                initial[p * 4 + 2] = UInt8(r)
                initial[p * 4 + 3] = UInt8(a)
            }
            staging = try gpu.buffer(initial)
            caseBuffer = try gpu.buffer(cases.map { SIMD2<UInt16>($0.0, $0.1) })
            halfBuffer = try gpu.buffer(halves)
            target = try gpu.texture(.bgra8Unorm, width, height, usage: [.renderTarget, .shaderRead])
        }

        public func state(_ pair: Pair) throws -> MTLRenderPipelineState {
            try gpu.render(fragment: "x1_blend_source", format: .bgra8Unorm) { descriptor in
                let attachment = descriptor.colorAttachments[0]!
                attachment.isBlendingEnabled = true
                attachment.sourceRGBBlendFactor = pair.source.0
                attachment.sourceAlphaBlendFactor = pair.source.0
                attachment.destinationRGBBlendFactor = pair.destination.0
                attachment.destinationAlphaBlendFactor = pair.destination.0
            }
        }

        public func encode(_ commands: MTLCommandBuffer, _ state: MTLRenderPipelineState) {
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: staging, sourceOffset: 0, sourceBytesPerRow: width * 4, sourceBytesPerImage: 0,
                sourceSize: MTLSize(width: width, height: height, depth: 1), to: target, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin())
            blit.endEncoding()
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBuffer(caseBuffer, offset: 0, index: 0)
            encoder.setFragmentBuffer(halfBuffer, offset: 0, index: 1)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }

        public func counts(_ pair: Pair) async throws -> Counts {
            compare(pair, try await gpu.read(target, bytesPerPixel: 4))
        }
    }

    /// The destination's codes at pixel p: (blue, green, red, alpha) in memory order.
    static func destination(_ p: Int) -> (Int, Int, Int, Int) {
        let d = p & 255
        return (255 - d, (d * 7 + 3) & 255, d, d)
    }

    public static func exact(_ gpu: GPU) async throws -> ProbeFinding {
        var wrong: [String: Int] = [:]
        var metrics = Metrics()
        metrics.set("rule", "fetch.blend")
        var paths: [String] = []
        var chipMisses = 0
        let rig = try Rig(gpu)
        for pair in pairs {
            let state = try rig.state(pair)
            paths.append(try await gpu.run { rig.encode($0, state) }.path)
            let counts = try await rig.counts(pair)
            wrong[pair.name + "_wrong"] = counts.separate
            chipMisses += counts.chip
            metrics.set(pair.name + "_chip_misses", counts.chip)
            metrics.set(pair.name + "_fma_misses", counts.fma)
            metrics.set(pair.name + "_exact_misses", counts.exact)
            metrics.set(pair.name + "_real_misses", counts.real)
            metrics.set(pair.name + "_per_product_misses", counts.perProduct)
            metrics.set(pair.name + "_max_diff", counts.maxDiff)
            metrics.set(pair.name + "_up_down", "\(counts.up):\(counts.down)")
            if let first = counts.first { metrics.set(pair.name + "_first", first) }
        }
        metrics.set("model", chipMisses == 0 ? "chip" : "none")
        metrics.set("chip_misses", chipMisses)
        return ProbeFinding.counted(wrong, paths: paths, metrics: metrics)
    }

    /// Per channel against Metal's half arithmetic (separate, fma, exact on the half inputs), the chip's blend (the
    /// model the device's counts single out), each product rounded alone, and the real result of the inputs; up and
    /// down count channels above and below Metal's separate code.
    public struct Counts: Sendable {
        public var fma = 0, separate = 0, exact = 0, chip = 0, perProduct = 0, real = 0, up = 0, down = 0, maxDiff = 0
        public var first: String?

        mutating func add(
            _ have: Int, _ s: Double, _ sa: Double, _ dc: Int, _ da: Int, _ pair: Pair, p: Int, channel: Int
        ) {
            let texel = X1BlendProbes.texels[dc], daTexel = X1BlendProbes.texels[da]
            let fs = pair.source.1.value(sa: sa, da: daTexel), fd = pair.destination.1.value(sa: sa, da: daTexel)
            let want = X1Reference.unorm(X1Reference.halfSeparate(s, fs, texel, fd), top: 255)
            if have != want {
                separate += 1
                if have > want { up += 1 } else { down += 1 }
                if first == nil { first = "\(p).\(channel):\(have),want_\(want)" }
            }
            if have != X1Reference.unorm(X1Reference.halfFMA(s, fs, texel, fd), top: 255) { fma += 1 }
            if have != X1Reference.unorm(X1Reference.exact(s, fs, texel, fd), top: 255) { exact += 1 }
            let s8 = X1Reference.store(s), sa8 = X1Reference.store(sa)
            let f1 = pair.source.1.code(sa: sa8, da: da), f2 = pair.destination.1.code(sa: sa8, da: da)
            if have != X1Reference.chipBlend(s8, f1, dc, f2) { chip += 1 }
            if have != X1Reference.perProductBlend(s8, f1, dc, f2) { perProduct += 1 }
            let real =
                s * pair.source.1.exact(sa: sa, da: da) + Double(dc) / 255 * pair.destination.1.exact(sa: sa, da: da)
            if have != X1Reference.unorm(real, top: 255) { self.real += 1 }
            maxDiff = max(maxDiff, abs(have - want))
        }
    }

    /// Every channel of every case against every model's code.
    static func compare(_ pair: Pair, _ bytes: [UInt8]) -> Counts {
        var counts = Counts()
        let halfValues = halves.map(Double.init)
        for p in 0..<(cases.count * 256) {
            let (si, ai) = cases[p >> 8]
            let (s, sa) = (halfValues[Int(si)], halfValues[Int(ai)])
            let (b, g, r, a) = destination(p)
            counts.add(Int(bytes[p * 4]), s, sa, b, a, pair, p: p, channel: 0)
            counts.add(Int(bytes[p * 4 + 1]), s, sa, g, a, pair, p: p, channel: 1)
            counts.add(Int(bytes[p * 4 + 2]), s, sa, r, a, pair, p: p, channel: 2)
            counts.add(Int(bytes[p * 4 + 3]), sa, sa, a, a, pair, p: p, channel: 3)
        }
        return counts
    }
}
