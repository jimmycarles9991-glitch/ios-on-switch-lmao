import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Draws Metal's validation stops fail their command buffer on the raw path with the raw path's own reason, never
/// handed to m2v (its error then names m2v): a tessellation state drawn as primitives, a plain state drawn as patches,
/// an amplification count above the state's maxVertexAmplificationCount (1 for a state that sets none) and patches at a
/// tessellation factor scale of 0. An M4 Max's validation layer stops each of these draws.
struct DrawErrorsTest: HWTest {
    let id = "gpu.raw.draw_errors"
    let name = "Draws Metal's validation stops fail their command buffer with the raw path's reason, not on m2v"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        [[patch(triangle, 3)]] vertex float4 errors_post(float3 uvw [[position_in_patch]]) {
            return float4(uvw.xy * 2 - 1, 0, 1);
        }
        vertex float4 errors_vertex(uint v [[vertex_id]]) {
            const float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
            return float4(p[v], 0, 1);
        }
        fragment float4 errors_fragment() { return float4(1); }
        """

    static let internalError = "MTLCommandBufferErrorDomain/\(MTLCommandBufferError.internal.rawValue) "

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        func state(_ vertex: String) throws -> MTLRenderPipelineState {
            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = library.makeFunction(name: vertex)
            descriptor.fragmentFunction = library.makeFunction(name: "errors_fragment")
            descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
            return try gpu.device.makeRenderPipelineState(descriptor: descriptor)
        }
        let tessellated = try state("errors_post")
        let plain = try state("errors_vertex")
        let factors = try gpu.buffer(256)
        let target = try gpu.texture(.rgba8Unorm, 8, 8)
        let arms: [(String, String, (MTLRenderCommandEncoder) -> Void)] = [
            (
                "tess_as_primitives", "primitives drawn with a tessellation state",
                { encoder in
                    encoder.setRenderPipelineState(tessellated)
                    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                }
            ),
            (
                "plain_as_patches", "patches drawn with a state of no post-tessellation vertex function",
                { encoder in
                    encoder.setRenderPipelineState(plain)
                    encoder.setTessellationFactorBuffer(factors, offset: 0, instanceStride: 0)
                    encoder.drawPatches(
                        numberOfPatchControlPoints: 3, patchStart: 0, patchCount: 1, patchIndexBuffer: nil,
                        patchIndexBufferOffset: 0, instanceCount: 1, baseInstance: 0)
                }
            ),
            (
                "scale_zero", "a tessellation factor scale that is not a positive half",
                { encoder in
                    encoder.setRenderPipelineState(tessellated)
                    encoder.setTessellationFactorBuffer(factors, offset: 0, instanceStride: 0)
                    encoder.setTessellationFactorScale(0)
                    encoder.drawPatches(
                        numberOfPatchControlPoints: 3, patchStart: 0, patchCount: 1, patchIndexBuffer: nil,
                        patchIndexBufferOffset: 0, instanceCount: 1, baseInstance: 0)
                }
            ),
            (
                "amplification_above_max", "a vertex amplification count above the state's maxVertexAmplificationCount",
                { encoder in
                    encoder.setRenderPipelineState(plain)
                    var maps = [MTLVertexAmplificationViewMapping](
                        repeating: MTLVertexAmplificationViewMapping(
                            viewportArrayIndexOffset: 0, renderTargetArrayIndexOffset: 0), count: 2)
                    encoder.setVertexAmplificationCount(2, viewMappings: &maps)
                    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                }
            ),
        ]
        var metrics = Metrics()
        var wrong: [String] = []
        for (label, why, draw) in arms {
            let timing = try await gpu.run(tolerateError: true) { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                draw(encoder)
                encoder.endEncoding()
            }
            metrics.set(label, String(timing.path.prefix(160)))
            if timing.path != "error:" + Self.internalError + why { wrong.append(label) }
        }
        return wrong.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(wrong.joined(separator: ","), metrics)
    }
}
