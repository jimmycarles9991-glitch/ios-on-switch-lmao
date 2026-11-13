import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A render encoder the raw path refuses (an infinite depth bias, which only m2v takes) that holds a patch draw, which m2v
/// has no form of: its command buffer fails with an error naming both, as Metal reports a failed command buffer, never
/// drawn without the patches. The same refusal beside a plain draw still completes on m2v.
struct NativeM2vMissingTest: NativeObjectTest {
    let id = "metal.native.m2v_missing"
    let name = "A command buffer left to m2v with patches in it fails with its error, never drawn without them"

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct ControlPoint { float4 position [[attribute(0)]]; };
        struct QuadPatch { patch_control_point<ControlPoint> points; };
        [[patch(quad, 4)]] vertex float4 missing_post(QuadPatch patch [[stage_in]], float2 uv [[position_in_patch]]) {
            const float4 low = mix(patch.points[0].position, patch.points[1].position, uv.x);
            const float4 high = mix(patch.points[3].position, patch.points[2].position, uv.x);
            return mix(low, high, uv.y);
        }
        vertex float4 missing_quad(uint v [[vertex_id]]) {
            return float4(float(v & 1) * 2 - 1, float(v >> 1) * 2 - 1, 0, 1);
        }
        fragment float4 missing_green() { return float4(0, 1, 0, 1); }
        """

    static let corners: [SIMD4<Float>] = [
        SIMD4(-1, -1, 0, 1), SIMD4(1, -1, 0, 1), SIMD4(1, 1, 0, 1), SIMD4(-1, 1, 0, 1),
    ]

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        let vertices = MTLVertexDescriptor()
        vertices.attributes[0].format = .float4
        vertices.attributes[0].bufferIndex = 0
        vertices.layouts[0].stride = MemoryLayout<SIMD4<Float>>.stride
        vertices.layouts[0].stepFunction = .perPatchControlPoint
        let patches = MTLRenderPipelineDescriptor()
        patches.vertexFunction = library.makeFunction(name: "missing_post")
        patches.fragmentFunction = library.makeFunction(name: "missing_green")
        patches.colorAttachments[0].pixelFormat = .rgba8Unorm
        patches.vertexDescriptor = vertices
        patches.tessellationPartitionMode = .integer
        patches.maxTessellationFactor = 16
        let plain = MTLRenderPipelineDescriptor()
        plain.vertexFunction = library.makeFunction(name: "missing_quad")
        plain.fragmentFunction = library.makeFunction(name: "missing_green")
        plain.colorAttachments[0].pixelFormat = .rgba8Unorm
        let tessellation = try await device.makeRenderPipelineState(descriptor: patches)
        let quad = try await device.makeRenderPipelineState(descriptor: plain)
        let points = try gpu.buffer(Self.corners)
        let factors = try gpu.buffer([Float16](repeating: 4, count: 6))

        for withPatches in [true, false] {
            let target = try gpu.texture(.rgba8Unorm, 16, 16)
            let timing = try await gpu.run(tolerateError: true) { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setDepthBias(.infinity, slopeScale: 0, clamp: 0)
                if withPatches {
                    encoder.setRenderPipelineState(tessellation)
                    encoder.setVertexBuffer(points, offset: 0, index: 0)
                    encoder.setTessellationFactorBuffer(factors, offset: 0, instanceStride: 0)
                    encoder.drawPatches(
                        numberOfPatchControlPoints: 4, patchStart: 0, patchCount: 1, patchIndexBuffer: nil,
                        patchIndexBufferOffset: 0, instanceCount: 1, baseInstance: 0)
                } else {
                    encoder.setRenderPipelineState(quad)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            let failed = timing.path.hasPrefix("error:")
            let label = withPatches ? "patches" : "plain"
            metrics.set("\(label)_path", failed ? "error" : timing.path)
            if withPatches {
                if !failed {
                    work.failures.append("a refused encoder of patches completed on \(timing.path)")
                } else if !timing.path.contains("which m2v does not draw") || !timing.path.contains("depth bias") {
                    work.failures.append("its error names neither the refusal nor what m2v lacks: \(timing.path)")
                }
            } else if failed {
                work.failures.append("a refused encoder of a plain draw failed: \(timing.path)")
            }
        }
        return work
    }
}
