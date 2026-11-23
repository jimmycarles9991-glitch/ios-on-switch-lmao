import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Patch commands of a state whose set 0 is past one 256-byte block: the post-tessellation vertex function reads 20
/// buffers of its own besides its control points, so set 0 holds over 16 descriptors and the control stage's buffers
/// sit in its second block. The patch macro selects the set at the state's own size. One quad patch at level 4 over the
/// left half of the target, every buffer holding 0 so the position is the patch's own: the left half green, the right
/// half black.
struct PatchCommandsWideSetTest: HWTest {
    let id = "gpu.gm20b.patch_commands_wide_set"
    let name = "Patch commands of a state whose set 0 is past 256 bytes draw on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let size = 32
    static let buffers = 2 ..< 22

    static var source: String {
        let parameters = buffers.map { "device const float *b\($0) [[buffer(\($0))]]" }.joined(separator: ", ")
        let sum = buffers.map { "b\($0)[0]" }.joined(separator: " + ")
        return """
        #include <metal_stdlib>
        using namespace metal;
        struct WideControlPoint { float4 position [[attribute(0)]]; };
        struct WideQuadPatch { patch_control_point<WideControlPoint> points; };
        [[patch(quad, 4)]] vertex float4 wide_set_post(WideQuadPatch patch [[stage_in]],
                                                      float2 uv [[position_in_patch]], \(parameters)) {
            const float4 low = mix(patch.points[0].position, patch.points[1].position, uv.x);
            const float4 high = mix(patch.points[3].position, patch.points[2].position, uv.x);
            return mix(low, high, uv.y) + float4(\(sum), 0, 0, 0);
        }
        fragment float4 wide_set_green() { return float4(0, 1, 0, 1); }
        """
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let device = gpu.device
        let indirect =
            (device as AnyObject as! NSObject).value(forKey: "supportsIndirectTessellation") as? Bool ?? false
        guard indirect else { return .skip("the device answers supportsIndirectTessellation NO") }
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        let vertices = MTLVertexDescriptor()
        vertices.attributes[0].format = .float4
        vertices.attributes[0].bufferIndex = 0
        vertices.layouts[0].stride = MemoryLayout<SIMD4<Float>>.stride
        vertices.layouts[0].stepFunction = .perPatchControlPoint
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "wide_set_post")
        descriptor.fragmentFunction = library.makeFunction(name: "wide_set_green")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.vertexDescriptor = vertices
        descriptor.tessellationPartitionMode = .integer
        descriptor.tessellationFactorStepFunction = .perPatch
        descriptor.maxTessellationFactor = 16
        descriptor.supportIndirectCommandBuffers = true
        let pipeline = try await device.makeRenderPipelineState(descriptor: descriptor)
        let points = try gpu.buffer([
            SIMD4<Float>(-1, -1, 0, 1), SIMD4<Float>(0, -1, 0, 1), SIMD4<Float>(0, 1, 0, 1), SIMD4<Float>(-1, 1, 0, 1),
        ])
        let factors = try gpu.buffer([Float16](repeating: 4, count: 6))
        let zeros = try gpu.buffer([Float](repeating: 0, count: 4))
        let icb = MTLIndirectCommandBufferDescriptor()
        icb.commandTypes = [.drawPatches]
        icb.inheritPipelineState = true
        icb.inheritBuffers = true
        guard let commands = device.makeIndirectCommandBuffer(descriptor: icb, maxCommandCount: 1, options: [])
        else { throw TestFailure("makeIndirectCommandBuffer returned nil for patch commands") }
        commands.indirectRenderCommandAt(0).drawPatches(
            4, patchStart: 0, patchCount: 1, patchIndexBuffer: nil, patchIndexBufferOffset: 0, instanceCount: 1,
            baseInstance: 0, tessellationFactorBuffer: factors, tessellationFactorBufferOffset: 0,
            tessellationFactorBufferInstanceStride: 0
        )
        let target = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
        let timing = try await gpu.run(tolerateError: true) { buffer in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = buffer.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setVertexBuffer(points, offset: 0, index: 0)
            for index in Self.buffers { encoder.setVertexBuffer(zeros, offset: 0, index: index) }
            encoder.setTessellationFactorBuffer(factors, offset: 0, instanceStride: 0)
            encoder.useResource(factors, usage: .read, stages: .vertex)
            encoder.executeCommandsInBuffer(commands, range: 0 ..< 1)
            encoder.endEncoding()
        }
        var metrics = Metrics()
        metrics.set("path", String(timing.path.prefix(160)))
        var wrong = -1
        if !timing.path.hasPrefix("error:") {
            let pixels = try await gpu.read(target, bytesPerPixel: 4)
            wrong =
                (0 ..< Self.size * Self.size).filter { i in
                    let want: [UInt8] = i % Self.size < Self.size / 2 ? [0, 255, 0, 255] : [0, 0, 0, 255]
                    return Array(pixels.pixel(i % Self.size, i / Self.size, width: Self.size)) != want
                }.count
        }
        metrics.set("wrong_pixels", wrong)
        return timing.path == "gm20b" && wrong == 0
            ? .pass(metrics, proof: gpu.proof) : .fail("path \(timing.path.prefix(80)), \(wrong) pixels wrong", metrics)
    }
}
