import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The units of a constant depth bias (setDepthBias:slopeScale:clamp:) on the driver: a flat quad at z drawn with bias b
/// over depth cleared to 0. A Depth16Unorm beside a Stencil8 (rendered through a Depth32Float_Stencil8 with its depth
/// converted) moves the stored code by b codes, as Metal's own Depth16Unorm does; a Depth32Float, alone or with stencil,
/// by b units of 2^(e-23), e the exponent of z. The expected values are an M4 Max's (macOS 27.2.1):
/// Depth16Unorm at 0.5 moves +1, +100, +1000 and -102 codes for 1, 100, 1000 and -100, within 2 codes here (the
/// conversion out of the float depth rounds); Depth32Float moves 50 units of 2^-24 for 100 at 0.25 and 100 at 0.5. Every
/// pass must stay raw.
struct DepthBiasUnitsTest: HWTest {
    let id = "gpu.gm20b.depth_bias_units"
    let name = "Depth bias in Metal's units of each depth format, a Depth16Unorm pair's included, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let n = 16

    struct Arm {
        let depth: MTLPixelFormat
        let stencil: Bool
        let z: Float
        let bias: Float
        let expected: Double // codes for Depth16Unorm, units of 2^-24 for the float formats
        let tolerance: Double
    }

    static let arms: [Arm] = [
        Arm(depth: .depth16Unorm, stencil: true, z: 0.5, bias: 1, expected: 1, tolerance: 2),
        Arm(depth: .depth16Unorm, stencil: true, z: 0.5, bias: 100, expected: 100, tolerance: 2),
        Arm(depth: .depth16Unorm, stencil: true, z: 0.5, bias: 1000, expected: 1000, tolerance: 2),
        Arm(depth: .depth16Unorm, stencil: true, z: 0.5, bias: -100, expected: -102, tolerance: 2),
        Arm(depth: .depth32Float, stencil: false, z: 0.25, bias: 100, expected: 50, tolerance: 0.5),
        Arm(depth: .depth32Float, stencil: false, z: 0.5, bias: 100, expected: 100, tolerance: 0.5),
        Arm(depth: .depth32Float_stencil8, stencil: false, z: 0.25, bias: 100, expected: 50, tolerance: 0.5),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let n = Self.n
        let colour = try gpu.texture(.rgba8Unorm, n, n)
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .always
        d.isDepthWriteEnabled = true
        guard let always = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
        var paths: [String] = [], moved: [String] = [], failures: [String] = []
        for arm in Self.arms {
            let sixteen = arm.depth == .depth16Unorm
            let combined = arm.depth == .depth32Float_stencil8
            let depth = try gpu.texture(arm.depth, n, n, usage: [.renderTarget])
            let stencil = arm.stencil ? try gpu.texture(.stencil8, n, n, usage: [.renderTarget]) : nil
            let state = try gpu.render(
                fragment: "solid_fragment", format: .rgba8Unorm, depth: arm.depth,
                stencil: arm.stencil ? .stencil8 : combined ? arm.depth : .invalid
            )
            let texel = sixteen ? 2 : 4
            let out = try gpu.buffer(n * n * texel)
            func value(_ bias: Float) async throws -> (Double, String) {
                let path = try await gpu.run { commands in
                    let p = MTLRenderPassDescriptor()
                    p.colorAttachments[0].texture = colour
                    p.colorAttachments[0].loadAction = .clear
                    p.colorAttachments[0].storeAction = .store
                    p.depthAttachment.texture = depth
                    p.depthAttachment.loadAction = .clear
                    p.depthAttachment.clearDepth = 0
                    p.depthAttachment.storeAction = .store
                    if let s = stencil ?? (combined ? depth : nil) {
                        p.stencilAttachment.texture = s
                        p.stencilAttachment.loadAction = .clear
                        p.stencilAttachment.storeAction = .store
                    }
                    let e = commands.makeRenderCommandEncoder(descriptor: p)!
                    e.setRenderPipelineState(state)
                    e.setDepthStencilState(always)
                    e.setDepthBias(bias, slopeScale: 0, clamp: 0)
                    var corners: [SIMD4<Float>] = [
                        [-1, -1, arm.z, 1], [1, -1, arm.z, 1], [-1, 1, arm.z, 1], [1, 1, arm.z, 1],
                    ]
                    e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    var red = SIMD4<Float>(1, 0, 0, 1)
                    e.setFragmentBytes(&red, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
                    e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    e.endEncoding()
                    let b = commands.makeBlitCommandEncoder()!
                    b.copy(
                        from: depth, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                        sourceSize: MTLSize(width: n, height: n, depth: 1), to: out, destinationOffset: 0,
                        destinationBytesPerRow: n * texel, destinationBytesPerImage: n * n * texel,
                        options: combined ? .depthFromDepthStencil : []
                    )
                    b.endEncoding()
                }.path
                let at = (n / 2) * (n + 1)
                if sixteen { return (Double(out.contents().load(fromByteOffset: 2 * at, as: UInt16.self)), path) }
                return (Double(out.contents().load(fromByteOffset: 4 * at, as: Float.self)) * 16_777_216, path)
            }
            let (base, basePath) = try await value(0)
            let (biased, biasedPath) = try await value(arm.bias)
            paths += [basePath, biasedPath]
            let delta = biased - base
            let label = "\(arm.depth.rawValue)\(arm.stencil ? "+s8" : "")@\(arm.z)/\(Int(arm.bias))"
            moved.append(String(format: "%@=%.2f", label, delta))
            if abs(delta - arm.expected) > arm.tolerance {
                failures.append(String(format: "%@ moved %.2f, Metal %.0f", label, delta, arm.expected))
            }
        }
        var metrics = Metrics()
        metrics.set("paths", paths.joined(separator: ","))
        metrics.set("moved", moved.joined(separator: " "))
        if paths.contains(where: { $0 != "gm20b" }) { failures.append("a pass went off the driver") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
