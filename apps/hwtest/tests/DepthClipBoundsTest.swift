import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The depths Metal's draws store into Depth32Float on the driver, against an M4 Max's (macOS 27.2.1): in clip mode
/// every depth within 0 to 1, whether a finite bias, a viewport depth range past 1 or a fragment function's own depth
/// takes it out; in clamp mode from the viewport's lower depth but at least 0 to its higher, a fragment function's own
/// depth to 1 at most. A flat triangle over the target, its depth read back at the middle.
///
/// The `per_vertex` rows measure what GM20B stores where Metal clamps each vertex's depth to 0..1 before its bias, which no
/// GM20B register reproduces (its depth clamp, MIN_Z_ZERO_MAX_Z_ONE and the bias clamp act on the biased pixel depth):
/// viewport ranges A -0.5..1.5, B 1.5..-0.5 and C 0.25..1.75, flat at z 0 or 1, under biases 0, -1e7, +1e7 (u: clamped
/// to 0.25), 3e6 and slopes +4 and -4, in clip (p) and clamp (k) mode. Each row holds GM20B's value, Metal's (an M4 Max's)
/// and the value raw's model predicts (m2vf-refusals2 probe/boundsmodel2.py: the pixel clamp of the unclamped biased depth,
/// the clamp's top 1 plus the most a constant bias adds where that is known); the case fails where GM20B is not the
/// model's, so a boot measures the model. 9 of the 22 meet Metal's: unbiased and non-negative constant biases above 1.
struct DepthClipBoundsTest: HWTest {
    let id = "gpu.gm20b.depth_clip_bounds"
    let name = "Depth32Float depths stay within Metal's bounds in clip and clamp mode on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let n = 16

    struct Arm {
        let label: String
        var clamp = false
        var range: (Double, Double) = (0, 1)
        var z: Float = 0.5
        var bias: Float = 0
        var fragmentDepth: Float? = nil
        let metal: Double
    }

    static let arms: [Arm] = [
        Arm(label: "clip/bias+", bias: 1e7, metal: 1),
        Arm(label: "clip/bias-", bias: -1e7, metal: 0),
        Arm(label: "clip/far1.75", range: (0.25, 1.75), z: 0.75, metal: 1),
        Arm(label: "clip/fragment1.5", fragmentDepth: 1.5, metal: 1),
        Arm(label: "clamp/near-.5/bias-", clamp: true, range: (-0.5, 1.5), bias: -1e7, metal: 0),
        Arm(label: "clamp/fragment1.5", clamp: true, range: (0.25, 1.75), fragmentDepth: 1.5, metal: 1),
        Arm(label: "clamp/far1.75/bias+", clamp: true, range: (0.25, 1.75), bias: 3e6, metal: 1.3576279),
    ]

    struct Row {
        let label: String
        let clamp: Bool
        let range: (Double, Double)
        let z: Float
        let bias: (Float, Float, Float)
        let metal: Double
        let model: Double
    }

    static let rows: [Row] = {
        let a = (-0.5, 1.5), b = (1.5, -0.5), c = (0.25, 1.75)
        let biases: [(String, (Float, Float, Float))] = [
            ("0", (0, 0, 0)), ("-", (-1e7, 0, 0)), ("s+", (0, 4, 0)), ("s-", (0, -4, 0)), ("u", (1e7, 0, 0.25)),
            ("3m", (3e6, 0, 0)),
        ]
        var rows = [
            Row(label: "pA0/+", clamp: false, range: a, z: 0, bias: (1e7, 0, 0), metal: 0, model: 0.0960464),
            Row(label: "pB1/+", clamp: false, range: b, z: 1, bias: (1e7, 0, 0), metal: 0, model: 0.0960464),
            Row(label: "kA0/+", clamp: true, range: a, z: 0, bias: (1e7, 0, 0), metal: 0, model: 0.0960464),
            Row(label: "kB1/+", clamp: true, range: b, z: 1, bias: (1e7, 0, 0), metal: 0, model: 0.0960464),
        ]
        let metal: [String: Double] = ["0": 1, "-": 0, "s+": 1, "s-": 1, "u": 1.25, "3m": 1.3576279]
        for (name, range, z, model, low) in [
            ("kC1", c, Float(1), 1.75, 0.25), ("kA1", a, 1, 1.5, 0), ("kB0", b, 0, 1.5, 0),
        ] {
            for (bias, value) in biases {
                let metalValue = bias == "-" ? low : metal[bias]!
                // the range's top meets 1 plus the most a known bias adds (depth_bounds.h topDepthBias)
                let known = bias == "0" || bias == "u" || bias == "3m"
                let biased = bias == "-" ? (name == "kC1" ? 0.5579071 : 0.3079071) : known ? metalValue : model
                rows.append(
                    Row(
                        label: "\(name)/\(bias)", clamp: true, range: range, z: z, bias: value, metal: metalValue,
                        model: biased
                    )
                )
            }
        }
        return rows
    }()

    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    vertex float4 bounds_vertex(uint v [[vertex_id]], constant float &z [[buffer(0)]]) {
        const float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
        return float4(p[v], z, 1);
    }
    fragment float4 bounds_colour() { return float4(1, 0, 0, 1); }
    struct Depth { float4 colour [[color(0)]]; float depth [[depth(any)]]; };
    fragment Depth bounds_depth(constant float &d [[buffer(0)]]) { return {float4(1, 0, 0, 1), d}; }
    """

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let n = Self.n
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        func state(_ fragment: String) throws -> MTLRenderPipelineState {
            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = library.makeFunction(name: "bounds_vertex")
            descriptor.fragmentFunction = library.makeFunction(name: fragment)
            descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
            descriptor.depthAttachmentPixelFormat = .depth32Float
            return try gpu.device.makeRenderPipelineState(descriptor: descriptor)
        }
        let plain = try state("bounds_colour"), writing = try state("bounds_depth")
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .always
        d.isDepthWriteEnabled = true
        guard let always = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
        let colour = try gpu.texture(.rgba8Unorm, n, n)
        var paths: [String] = [], values: [String] = [], failures: [String] = []
        for arm in Self.arms {
            let depth = try gpu.texture(.depth32Float, n, n, usage: [.renderTarget])
            let out = try gpu.buffer(n * n * 4)
            let path = try await gpu.run { commands in
                let p = MTLRenderPassDescriptor()
                p.colorAttachments[0].texture = colour
                p.colorAttachments[0].loadAction = .clear
                p.colorAttachments[0].storeAction = .store
                p.depthAttachment.texture = depth
                p.depthAttachment.loadAction = .clear
                p.depthAttachment.clearDepth = 0.5
                p.depthAttachment.storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: p)!
                e.setRenderPipelineState(arm.fragmentDepth == nil ? plain : writing)
                e.setDepthStencilState(always)
                e.setViewport(
                    MTLViewport(
                        originX: 0, originY: 0, width: Double(n), height: Double(n), znear: arm.range.0,
                        zfar: arm.range.1
                    )
                )
                if arm.clamp { e.setDepthClipMode(.clamp) }
                e.setDepthBias(arm.bias, slopeScale: 0, clamp: 0)
                var z = arm.z
                e.setVertexBytes(&z, length: 4, index: 0)
                if var fragment = arm.fragmentDepth { e.setFragmentBytes(&fragment, length: 4, index: 0) }
                e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                e.endEncoding()
                let b = commands.makeBlitCommandEncoder()!
                b.copy(
                    from: depth, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                    sourceSize: MTLSize(width: n, height: n, depth: 1), to: out, destinationOffset: 0,
                    destinationBytesPerRow: n * 4, destinationBytesPerImage: n * n * 4
                )
                b.endEncoding()
            }.path
            paths.append(path)
            let got = Double(out.contents().load(fromByteOffset: 4 * (n / 2) * (n + 1), as: Float.self))
            values.append(String(format: "%@=%.6f", arm.label, got))
            if abs(got - arm.metal) > 1e-6 { failures.append(arm.label) }
        }
        var measured: [String] = [], offModel: [String] = []
        var offMetal = 0
        for row in Self.rows {
            let depth = try gpu.texture(.depth32Float, n, n, usage: [.renderTarget])
            let out = try gpu.buffer(n * n * 4)
            paths.append(
                try await gpu.run { commands in
                    let p = MTLRenderPassDescriptor()
                    p.colorAttachments[0].texture = colour
                    p.colorAttachments[0].loadAction = .clear
                    p.colorAttachments[0].storeAction = .store
                    p.depthAttachment.texture = depth
                    p.depthAttachment.loadAction = .clear
                    p.depthAttachment.clearDepth = 0.5
                    p.depthAttachment.storeAction = .store
                    let e = commands.makeRenderCommandEncoder(descriptor: p)!
                    e.setRenderPipelineState(plain)
                    e.setDepthStencilState(always)
                    e.setViewport(
                        MTLViewport(
                            originX: 0, originY: 0, width: Double(n), height: Double(n), znear: row.range.0,
                            zfar: row.range.1
                        )
                    )
                    if row.clamp { e.setDepthClipMode(.clamp) }
                    e.setDepthBias(row.bias.0, slopeScale: row.bias.1, clamp: row.bias.2)
                    var z = row.z
                    e.setVertexBytes(&z, length: 4, index: 0)
                    e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                    e.endEncoding()
                    let b = commands.makeBlitCommandEncoder()!
                    b.copy(
                        from: depth, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                        sourceSize: MTLSize(width: n, height: n, depth: 1), to: out, destinationOffset: 0,
                        destinationBytesPerRow: n * 4, destinationBytesPerImage: n * n * 4
                    )
                    b.endEncoding()
                }.path
            )
            let got = Double(out.contents().load(fromByteOffset: 4 * (n / 2) * (n + 1), as: Float.self))
            measured.append(String(format: "%@=%.7f", row.label, got))
            if abs(got - row.model) > 1e-6 { offModel.append(row.label) }
            if abs(got - row.metal) > 1e-6 { offMetal += 1 }
        }
        if !offModel.isEmpty { failures.append("per_vertex") }
        var metrics = Metrics()
        metrics.set("values", values.joined(separator: " "))
        metrics.set("per_vertex_a", measured.prefix(11).joined(separator: " "))
        metrics.set("per_vertex_b", measured.dropFirst(11).joined(separator: " "))
        metrics.set("per_vertex_off_model", offModel.isEmpty ? "-" : offModel.joined(separator: ","))
        metrics.set("per_vertex_off_metal", "\(offMetal) of \(Self.rows.count)")
        metrics.set("paths", Set(paths).sorted().joined(separator: ","))
        if paths.contains(where: { $0 != "gm20b" }) { failures.append("paths") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: ","), metrics)
    }
}
