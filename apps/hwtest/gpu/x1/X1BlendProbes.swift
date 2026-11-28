import Foundation
import HWTestCore
import HWTestGPU
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
            destination: (.oneMinusSourceAlpha, .oneMinusSa)
        ),
        Pair(name: "add", source: (.one, .one), destination: (.one, .one)),
    ]

    static let halves: [Float16] = (0 ..< 256).map { Float16(Double($0) / 255) }

    /// The probe's target, its destination codes and the cases' buffers; one pair's draw (after the destination's
    /// copy) and its comparison
    public struct Rig {
        let gpu: GPU
        let staging: MTLBuffer, caseBuffer: MTLBuffer, halfBuffer: MTLBuffer
        let target: MTLTexture

        public init(_ gpu: GPU) throws {
            self.gpu = gpu
            var initial = [UInt8](repeating: 0, count: width * height * 4)
            for p in 0 ..< width * height {
                let (b, g, r, a) = X1Reference.blendDestination(p)
                initial[p * 4] = UInt8(b)
                initial[p * 4 + 1] = UInt8(g)
                initial[p * 4 + 2] = UInt8(r)
                initial[p * 4 + 3] = UInt8(a)
            }
            staging = try gpu.buffer(initial)
            caseBuffer = try gpu.buffer(X1Reference.blendCases.map { SIMD2<UInt16>($0.0, $0.1) })
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
                destinationLevel: 0, destinationOrigin: MTLOrigin()
            )
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

        public func counts(_ pair: Pair, models: X1Reference.BlendModels = .all) async throws -> Counts {
            let bytes = try await gpu.read(target, bytesPerPixel: 4)
            return bytes.withUnsafeBytes {
                X1Reference.blendCounts($0, source: pair.source.1, destination: pair.destination.1, models: models)
            }
        }
    }

    public typealias Counts = X1Reference.BlendCounts

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
}
