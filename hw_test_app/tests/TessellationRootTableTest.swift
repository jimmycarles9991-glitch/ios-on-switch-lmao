import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A post-tessellation vertex function that reads a colour through one pointer that is a bound buffer or an argument
/// buffer's pointer, on the raw path: m2v reaches the bound slot through the draw's root table, whose address the
/// evaluation program reads from a push word the bridge places as the vertex stage's (it dropped the hole of a
/// TESSELLATION_EVALUATION range, leaving the word unwritten). Two quad patches at factor 1, one per target, the first
/// through the argument buffer, the second through the slot. Runs last: on a build without the bridge's placement the
/// evaluation program reads a root table at whatever the word held, which can fault the channel.
struct TessellationRootTableTest: HWTest {
    let id = "gpu.raw.tess_root_table"
    let name = "A post-tessellation vertex function reads a bound buffer through the draw's root table on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let n = 8
    static let viaArguments: [UInt8] = [255, 0, 255, 255]
    static let viaSlot: [UInt8] = [0, 0, 255, 255]

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct ControlPoint { float4 position [[attribute(0)]]; };
        struct QuadPatch { patch_control_point<ControlPoint> points; };
        struct Arguments { constant float4 *colour [[id(0)]]; };
        struct Out { float4 position [[position]]; float4 colour; };
        [[patch(quad, 4)]] vertex Out tess_root_post(QuadPatch patch [[stage_in]], float2 uv [[position_in_patch]],
                                                     constant Arguments &arguments [[buffer(1)]],
                                                     constant float4 *direct [[buffer(2)]],
                                                     constant uint &throughArguments [[buffer(3)]]) {
            const float4 low = mix(patch.points[0].position, patch.points[1].position, uv.x);
            const float4 high = mix(patch.points[3].position, patch.points[2].position, uv.x);
            Out out;
            out.position = mix(low, high, uv.y);
            out.colour = *(throughArguments ? arguments.colour : direct);
            return out;
        }
        fragment float4 tess_root_colour(Out in [[stage_in]]) { return in.colour; }
        """

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let n = Self.n
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        guard let post = library.makeFunction(name: "tess_root_post") else { throw TestFailure("no post function") }
        let vertices = MTLVertexDescriptor()
        vertices.attributes[0].format = .float4
        vertices.attributes[0].bufferIndex = 0
        vertices.layouts[0].stride = MemoryLayout<SIMD4<Float>>.stride
        vertices.layouts[0].stepFunction = .perPatchControlPoint
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = post
        descriptor.fragmentFunction = library.makeFunction(name: "tess_root_colour")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.vertexDescriptor = vertices
        descriptor.tessellationPartitionMode = .integer
        descriptor.maxTessellationFactor = 16
        let state = try await gpu.device.makeRenderPipelineState(descriptor: descriptor)
        let pointed = try gpu.buffer([SIMD4<Float>(1, 0, 1, 1)])
        let direct = try gpu.buffer([SIMD4<Float>(0, 0, 1, 1)])
        let encoder = post.makeArgumentEncoder(bufferIndex: 1)
        let arguments = try gpu.buffer(max(encoder.encodedLength, 16))
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.setBuffer(pointed, offset: 0, index: 0)
        let points = try gpu.buffer([
            SIMD4<Float>(-1, -1, 0, 1), SIMD4<Float>(1, -1, 0, 1), SIMD4<Float>(1, 1, 0, 1), SIMD4<Float>(-1, 1, 0, 1),
        ])
        let factors = try gpu.buffer([Float16](repeating: 1, count: 6))
        let targets = [try gpu.texture(.rgba8Unorm, n, n), try gpu.texture(.rgba8Unorm, n, n)]
        let path = try await gpu.run { commands in
            for (k, target) in targets.enumerated() {
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let render = commands.makeRenderCommandEncoder(descriptor: pass)!
                render.setRenderPipelineState(state)
                render.useResource(pointed, usage: .read, stages: .vertex)
                render.setVertexBuffer(points, offset: 0, index: 0)
                render.setVertexBuffer(arguments, offset: 0, index: 1)
                render.setVertexBuffer(direct, offset: 0, index: 2)
                var throughArguments: UInt32 = k == 0 ? 1 : 0
                render.setVertexBytes(&throughArguments, length: 4, index: 3)
                render.setTessellationFactorBuffer(factors, offset: 0, instanceStride: 0)
                render.drawPatches(
                    numberOfPatchControlPoints: 4, patchStart: 0, patchCount: 1, patchIndexBuffer: nil,
                    patchIndexBufferOffset: 0, instanceCount: 1, baseInstance: 0)
                render.endEncoding()
            }
        }.path
        var metrics = Metrics()
        metrics.set("path", path)
        var failures: [String] = path == "raw" ? [] : ["path"]
        for (label, target, want) in [
            ("arguments", targets[0], Self.viaArguments), ("slot", targets[1], Self.viaSlot),
        ] {
            let pixels = try await gpu.read(target, bytesPerPixel: 4)
            var wrong = 0
            for y in 0..<n {
                for x in 0..<n where Array(pixels.pixel(x, y, width: n)) != want { wrong += 1 }
            }
            metrics.set("\(label)_wrong", wrong)
            metrics.set(
                "\(label)_middle", Array(pixels.pixel(n / 2, n / 2, width: n)).map(String.init).joined(separator: "/"))
            if wrong > 0 { failures.append(label) }
        }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: ","), metrics)
    }
}
