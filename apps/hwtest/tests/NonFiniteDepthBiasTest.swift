import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// setDepthBias:slopeScale:clamp: with infinite and NaN factors on the driver, against the depths an M4 Max stores
/// (macOS 27.2.1): an unbounded +inf or NaN bias puts a triangle's depth at the top of its clamp ([0, 1], or the viewport's
/// range in clamp mode) and -inf at the bottom; a clamp bounds an infinite bias to the depth plus the clamp; a NaN bias
/// takes the clamp's side; an infinite slope leaves a flat triangle NaN and a sloped one infinite; a tessellated quad
/// patch is biased as a triangle (the patch arms, whose encoders m2v never drew). `zero_corner` records,
/// without judging it, what the chip stores for a Depth32Float flat triangle at depth 0 under a -inf constant: Metal's unit
/// there is 0, so its bias is NaN and it stores 1, which no finite polygon offset gives (the raw form's answer is 0).
struct NonFiniteDepthBiasTest: HWTest {
    let id = "gpu.gm20b.nonfinite_depth_bias"
    let name = "Infinite and NaN depth bias factors store Metal's depths on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let n = 16

    enum Expect {
        case depth(Double)
        case offset(Double) // the unbiased depth plus this
    }

    struct Arm {
        let label: String
        var format: MTLPixelFormat = .depth32Float
        var z: (Float, Float) = (0.5, 0.5) // left and right edges: equal for a flat triangle
        var clampRange: (Double, Double)? = nil // MTLDepthClipModeClamp through this viewport range
        var patches = false // one quad patch over the target in place of the strip
        // A Depth16Unorm pair renders through a converted scratch.
        var stencil = false
        let bias: (Float, Float, Float)
        let expect: Expect
    }

    static let patchSource = """
    #include <metal_stdlib>
    using namespace metal;
    struct ControlPoint { float4 position [[attribute(0)]]; };
    struct QuadPatch { patch_control_point<ControlPoint> points; };
    [[patch(quad, 4)]] vertex float4 bias_patch(QuadPatch patch [[stage_in]], float2 uv [[position_in_patch]]) {
        const float4 low = mix(patch.points[0].position, patch.points[1].position, uv.x);
        const float4 high = mix(patch.points[3].position, patch.points[2].position, uv.x);
        return mix(low, high, uv.y);
    }
    fragment float4 bias_patch_red() { return float4(1, 0, 0, 1); }
    """

    static let sloped: (Float, Float) = (0.2, 0.8)
    static let arms: [Arm] = [
        Arm(label: "c+inf", bias: (.infinity, 0, 0), expect: .depth(1)),
        Arm(label: "c-inf", bias: (-.infinity, 0, 0), expect: .depth(0)),
        Arm(label: "cnan/cl.125", bias: (.nan, 0, 0.125), expect: .offset(0.125)),
        Arm(label: "c+inf/cl-.125", bias: (.infinity, 0, -0.125), expect: .depth(1)),
        Arm(label: "c-inf/cl-.125", z: sloped, bias: (-.infinity, 0, -0.125), expect: .offset(-0.125)),
        Arm(label: "k-inf", bias: (0, -.infinity, 0), expect: .depth(1)),
        Arm(label: "k-inf/sloped", z: sloped, bias: (0, -.infinity, 0), expect: .depth(0)),
        Arm(label: "k+inf/cl-.125", bias: (0, .infinity, -0.125), expect: .offset(-0.125)),
        Arm(label: "k+inf/cl-.125/sloped", z: sloped, bias: (0, .infinity, -0.125), expect: .depth(1)),
        Arm(label: "knan/cl-inf", bias: (0, .nan, -.infinity), expect: .depth(0)),
        Arm(label: "c+inf/clamp", clampRange: (0.2, 0.7), bias: (.infinity, 0, 0), expect: .depth(0.7)),
        Arm(label: "c-inf/clamp", clampRange: (0.7, 0.2), bias: (-.infinity, 0, 0), expect: .depth(0.2)),
        Arm(label: "d16/c+inf", format: .depth16Unorm, bias: (.infinity, 0, 0), expect: .depth(1)),
        Arm(label: "d16/k-inf/cl.125", format: .depth16Unorm, bias: (0, -.infinity, 0.125), expect: .offset(0.125)),
        Arm(label: "d16/c-inf/sloped", format: .depth16Unorm, z: sloped, bias: (-.infinity, 0, 0), expect: .depth(0)),
        Arm(label: "d16+s8/c+inf", format: .depth16Unorm, stencil: true, bias: (.infinity, 0, 0), expect: .depth(1)),
        Arm(
            label: "d16+s8/k+inf/cl-.125/sloped", format: .depth16Unorm, z: sloped, stencil: true,
            bias: (0, .infinity, -0.125), expect: .depth(1)
        ),
        Arm(
            label: "d16+s8/knan/cl.125", format: .depth16Unorm, stencil: true, bias: (0, .nan, 0.125),
            expect: .offset(0.125)
        ),
        Arm(label: "patch/c+inf", patches: true, bias: (.infinity, 0, 0), expect: .depth(1)),
        Arm(label: "patch/k-inf/sloped", z: sloped, patches: true, bias: (0, -.infinity, 0), expect: .depth(0)),
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
        var states: [String: MTLRenderPipelineState] = [:]
        var patchState: MTLRenderPipelineState?
        let factors = try gpu.buffer([Float16](repeating: 4, count: 6))

        func patchPipeline() async throws -> MTLRenderPipelineState {
            if let state = patchState { return state }
            let library = try await gpu.device.makeLibrary(source: Self.patchSource, options: nil)
            let vertices = MTLVertexDescriptor()
            vertices.attributes[0].format = .float4
            vertices.attributes[0].bufferIndex = 0
            vertices.layouts[0].stride = MemoryLayout<SIMD4<Float>>.stride
            vertices.layouts[0].stepFunction = .perPatchControlPoint
            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = library.makeFunction(name: "bias_patch")
            descriptor.fragmentFunction = library.makeFunction(name: "bias_patch_red")
            descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
            descriptor.depthAttachmentPixelFormat = .depth32Float
            descriptor.vertexDescriptor = vertices
            descriptor.tessellationPartitionMode = .integer
            descriptor.maxTessellationFactor = 16
            let state = try await gpu.device.makeRenderPipelineState(descriptor: descriptor)
            patchState = state
            return state
        }
        var paths: [String] = [], values: [String] = [], failures: [String] = []

        func stored(_ arm: Arm, _ bias: (Float, Float, Float)) async throws -> Double {
            let sixteen = arm.format == .depth16Unorm
            let depth = try gpu.texture(arm.format, n, n, usage: [.renderTarget])
            let stateKey = "\(arm.format.rawValue)\(arm.stencil ? "+s8" : "")"
            if states[stateKey] == nil {
                states[stateKey] = try gpu.render(
                    fragment: "solid_fragment", format: .rgba8Unorm, depth: arm.format,
                    stencil: arm.stencil ? .stencil8 : .invalid
                )
            }
            let state = arm.patches ? try await patchPipeline() : states[stateKey]!
            let stencil = arm.stencil ? try gpu.texture(.stencil8, n, n, usage: [.renderTarget]) : nil
            let points = try gpu.buffer([
                SIMD4<Float>(-1, -1, arm.z.0, 1), SIMD4(1, -1, arm.z.1, 1), SIMD4(1, 1, arm.z.1, 1),
                SIMD4(-1, 1, arm.z.0, 1),
            ])
            let texel = sixteen ? 2 : 4
            let out = try gpu.buffer(n * n * texel)
            let path = try await gpu.run { commands in
                let p = MTLRenderPassDescriptor()
                p.colorAttachments[0].texture = colour
                p.colorAttachments[0].loadAction = .clear
                p.colorAttachments[0].storeAction = .store
                p.depthAttachment.texture = depth
                p.depthAttachment.loadAction = .clear
                p.depthAttachment.clearDepth = 0.5
                p.depthAttachment.storeAction = .store
                if let stencil {
                    p.stencilAttachment.texture = stencil
                    p.stencilAttachment.loadAction = .clear
                    p.stencilAttachment.storeAction = .store
                }
                let e = commands.makeRenderCommandEncoder(descriptor: p)!
                e.setRenderPipelineState(state)
                e.setDepthStencilState(always)
                if let range = arm.clampRange {
                    e.setViewport(
                        MTLViewport(
                            originX: 0, originY: 0, width: Double(n), height: Double(n),
                            znear: range.0, zfar: range.1
                        )
                    )
                    e.setDepthClipMode(.clamp)
                }
                e.setDepthBias(bias.0, slopeScale: bias.1, clamp: bias.2)
                if arm.patches {
                    e.setVertexBuffer(points, offset: 0, index: 0)
                    e.setTessellationFactorBuffer(factors, offset: 0, instanceStride: 0)
                    e.drawPatches(
                        numberOfPatchControlPoints: 4, patchStart: 0, patchCount: 1, patchIndexBuffer: nil,
                        patchIndexBufferOffset: 0, instanceCount: 1, baseInstance: 0
                    )
                } else {
                    var corners: [SIMD4<Float>] = [
                        [-1, -1, arm.z.0, 1], [1, -1, arm.z.1, 1], [-1, 1, arm.z.0, 1], [1, 1, arm.z.1, 1],
                    ]
                    e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    var red = SIMD4<Float>(1, 0, 0, 1)
                    e.setFragmentBytes(&red, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
                    e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                e.endEncoding()
                let b = commands.makeBlitCommandEncoder()!
                b.copy(
                    from: depth, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                    sourceSize: MTLSize(width: n, height: n, depth: 1), to: out, destinationOffset: 0,
                    destinationBytesPerRow: n * texel, destinationBytesPerImage: n * n * texel
                )
                b.endEncoding()
            }.path
            paths.append(path)
            let at = (n / 2) * (n + 1)
            if sixteen { return Double(out.contents().load(fromByteOffset: 2 * at, as: UInt16.self)) / 65535 }
            return Double(out.contents().load(fromByteOffset: 4 * at, as: Float.self))
        }

        for arm in Self.arms {
            let got = try await stored(arm, arm.bias)
            let want: Double
            switch arm.expect {
            case .depth(let v): want = v
            case .offset(let o):
                let base = try await stored(arm, (0, 0, 0))
                want = min(max(base + o, 0), 1)
            }
            values.append(String(format: "%@=%.5f", arm.label, got))
            let tolerance = arm.format == .depth16Unorm ? 1.5 / 65535 : 1e-6
            if abs(got - want) > tolerance {
                failures.append(String(format: "%@ %.6f, Metal %.6f", arm.label, got, want))
            }
        }
        let corner = try await stored(
            Arm(label: "corner", z: (0, 0), bias: (-.infinity, 0, 0), expect: .depth(1)),
            (-.infinity, 0, 0)
        )
        var metrics = Metrics()
        metrics.set("values", values.joined(separator: " "))
        metrics.set("zero_corner", String(format: "%.5f (Metal 1)", corner))
        metrics.set("paths", Set(paths).sorted().joined(separator: ","))
        if paths.contains(where: { $0 != "gm20b" }) { failures.append("a pass went off the driver") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
