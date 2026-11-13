import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Quad patches through the hardware tessellator. A kernel writes each patch's factors in the same command buffer (patch 0
/// level 4 everywhere, patch 1 an edge factor 0, which culls it); the post-tessellation vertex function marks each domain
/// point it runs at in a 17x17 grid of sixteenths. Integer partitioning at level 4 runs exactly the 5x5 points on
/// multiples of 4/16, and the drawn pixels are patch 0's half of the target. A patch index buffer of {1, 0} draws patch
/// 1's control points with patch 0's factors (Metal's patch index addresses control points, the position the factors),
/// so the other half is drawn. Where the device answers supportsIndirectTessellation, an indirect draw of the same patch
/// from GPU-written arguments draws patch 0's half again, and so do the two forms as an indirect command buffer's patch
/// commands, each with its own factor buffer and patch index buffer, each executed alone by its range under the inherited
/// state, then both in one execute, which paints the whole target. Every command buffer stays raw.
struct NativeTessellationTest: NativeObjectTest {
    let id = "metal.native.tessellation"
    let name = "Quad patches tessellated in hardware from GPU-written factors, exact to Metal's integer partitioning"

    static let size = 32
    static let grid = 17
    static let level: Float = 4

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        kernel void quad_factors(device MTLQuadTessellationFactorsHalf *factors [[buffer(0)]],
                                 constant float &level [[buffer(1)]], uint patch [[thread_position_in_grid]]) {
            for (int e = 0; e < 4; e++)
                factors[patch].edgeTessellationFactor[e] = half(patch == 0 || e != 2 ? level : 0.0f);
            factors[patch].insideTessellationFactor[0] = half(level);
            factors[patch].insideTessellationFactor[1] = half(level);
        }
        kernel void patch_arguments(device MTLDrawPatchIndirectArguments *arguments [[buffer(0)]]) {
            arguments->patchCount = 1;
            arguments->instanceCount = 1;
            arguments->patchStart = 0;
            arguments->baseInstance = 0;
        }
        struct ControlPoint { float4 position [[attribute(0)]]; };
        struct QuadPatch { patch_control_point<ControlPoint> points; };
        [[patch(quad, 4)]] vertex float4 quad_post(QuadPatch patch [[stage_in]], float2 uv [[position_in_patch]],
                                                   device atomic_uint *seen [[buffer(1)]]) {
            const uint u = uint(round(uv.x * 16)), v = uint(round(uv.y * 16));
            atomic_store_explicit(&seen[v * 17 + u], 1, memory_order_relaxed);
            const float4 low = mix(patch.points[0].position, patch.points[1].position, uv.x);
            const float4 high = mix(patch.points[3].position, patch.points[2].position, uv.x);
            return mix(low, high, uv.y);
        }
        fragment float4 patch_green() { return float4(0, 1, 0, 1); }
        """

    /// Patch 0 the left half of the target, patch 1 the right, four corners each in clip space
    static let controlPoints: [SIMD4<Float>] = [
        SIMD4(-1, -1, 0, 1), SIMD4(0, -1, 0, 1), SIMD4(0, 1, 0, 1), SIMD4(-1, 1, 0, 1),
        SIMD4(0, -1, 0, 1), SIMD4(1, -1, 0, 1), SIMD4(1, 1, 0, 1), SIMD4(0, 1, 0, 1),
    ]

    /// Pixels that are not green in the drawn half (`left`, or both where `both`) and black in the other
    static func wrong(_ pixels: [UInt8], left: Bool, both: Bool = false) -> Int {
        (0..<size * size).filter { i in
            let drawn = both || (i % size < size / 2) == left
            let want: [UInt8] = drawn ? [0, 255, 0, 255] : [0, 0, 0, 255]
            return Array(pixels.pixel(i % size, i / size, width: size)) != want
        }.count
    }

    /// Grid cells marked other than integer partitioning's points at `level`: missing ones and extra ones
    static func wrongPoints(_ seen: MTLBuffer) -> Int {
        let marks = seen.contents().bindMemory(to: UInt32.self, capacity: grid * grid)
        let step = Int(16 / level)
        return (0..<grid * grid).filter { i in
            let expected = (i % grid) % step == 0 && (i / grid) % step == 0
            return (marks[i] != 0) != expected
        }.count
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        guard let post = library.makeFunction(name: "quad_post"),
            let factorsKernel = library.makeFunction(name: "quad_factors"),
            let argumentsKernel = library.makeFunction(name: "patch_arguments")
        else { throw TestFailure("missing tessellation functions") }
        let vertices = MTLVertexDescriptor()
        vertices.attributes[0].format = .float4
        vertices.attributes[0].bufferIndex = 0
        vertices.layouts[0].stride = MemoryLayout<SIMD4<Float>>.stride
        vertices.layouts[0].stepFunction = .perPatchControlPoint
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = post
        descriptor.fragmentFunction = library.makeFunction(name: "patch_green")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.vertexDescriptor = vertices
        descriptor.tessellationPartitionMode = .integer
        descriptor.tessellationFactorStepFunction = .perPatch
        descriptor.maxTessellationFactor = 16
        descriptor.supportIndirectCommandBuffers = true
        let pipeline = try await device.makeRenderPipelineState(descriptor: descriptor)
        let factorState = try await device.makeComputePipelineState(function: factorsKernel)
        let argumentState = try await device.makeComputePipelineState(function: argumentsKernel)
        work.objects += [pipeline, factorState]

        let points = try gpu.buffer(Self.controlPoints)
        guard let factors = device.makeBuffer(length: 2 * 12, options: .storageModePrivate),
            let arguments = device.makeBuffer(length: 16, options: .storageModePrivate)
        else { throw TestFailure("no private factor or argument buffer") }
        let patchOrder = try gpu.buffer([UInt32](arrayLiteral: 1, 0))
        let indirect =
            (device as AnyObject as! NSObject).value(forKey: "supportsIndirectTessellation") as? Bool ?? false
        metrics.set("indirect", indirect ? "yes" : "no")

        let patchCommands = indirect ? try Self.commands(device, factors, patchOrder) : nil
        if let patchCommands { work.objects.append(patchCommands) }
        for form in ["direct", "indexed"] + (indirect ? ["indirect", "icb", "icb_indexed", "icb_both"] : []) {
            let target = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
            let seen = try gpu.buffer([UInt32](repeating: 0, count: Self.grid * Self.grid))
            let timing = try await gpu.run { commands in
                let compute = commands.makeComputeCommandEncoder()!
                compute.setComputePipelineState(factorState)
                compute.setBuffer(factors, offset: 0, index: 0)
                var level = Self.level
                compute.setBytes(&level, length: 4, index: 1)
                compute.dispatchThreadgroups(
                    MTLSize(width: 2, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
                compute.setComputePipelineState(argumentState)
                compute.setBuffer(arguments, offset: 0, index: 0)
                compute.dispatchThreadgroups(
                    MTLSize(width: 1, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
                compute.endEncoding()
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                encoder.setVertexBuffer(points, offset: 0, index: 0)
                encoder.setVertexBuffer(seen, offset: 0, index: 1)
                encoder.setTessellationFactorBuffer(factors, offset: 0, instanceStride: 0)
                switch form {
                case "direct":
                    encoder.drawPatches(
                        numberOfPatchControlPoints: 4, patchStart: 0, patchCount: 2, patchIndexBuffer: nil,
                        patchIndexBufferOffset: 0, instanceCount: 1, baseInstance: 0)
                case "indexed":
                    encoder.drawPatches(
                        numberOfPatchControlPoints: 4, patchStart: 0, patchCount: 1, patchIndexBuffer: patchOrder,
                        patchIndexBufferOffset: 0, instanceCount: 1, baseInstance: 0)
                case "icb", "icb_indexed", "icb_both":
                    encoder.useResource(factors, usage: .read, stages: .vertex)
                    encoder.useResource(patchOrder, usage: .read, stages: .vertex)
                    let range = form == "icb" ? 0..<1 : form == "icb_indexed" ? 1..<2 : 0..<2
                    encoder.executeCommandsInBuffer(patchCommands!, range: range)
                default:
                    encoder.drawPatches(
                        numberOfPatchControlPoints: 4, patchIndexBuffer: nil, patchIndexBufferOffset: 0,
                        indirectBuffer: arguments, indirectBufferOffset: 0)
                }
                encoder.endEncoding()
            }
            work.paths.append(timing.path)
            let wrongPixels = Self.wrong(
                try await gpu.read(target, bytesPerPixel: 4), left: !form.hasSuffix("indexed"), both: form == "icb_both"
            )
            let wrongPoints = Self.wrongPoints(seen)
            metrics.set("\(form)_wrong_pixels", wrongPixels)
            metrics.set("\(form)_wrong_points", wrongPoints)
            if wrongPixels != 0 { work.failures.append("\(form): \(wrongPixels) pixels wrong") }
            if wrongPoints != 0 { work.failures.append("\(form): \(wrongPoints) domain points wrong at level 4") }
        }
        return work
    }

    /// Patch commands of an indirect command buffer that inherits its state: both patches from the factors (patch 1 is
    /// culled), then patch 0's factors over patch 1's control points through the patch index buffer
    static func commands(
        _ device: MTLDevice, _ factors: MTLBuffer, _ patchOrder: MTLBuffer
    ) throws
        -> MTLIndirectCommandBuffer
    {
        let descriptor = MTLIndirectCommandBufferDescriptor()
        descriptor.commandTypes = [.drawPatches]
        descriptor.inheritPipelineState = true
        descriptor.inheritBuffers = true
        guard let commands = device.makeIndirectCommandBuffer(descriptor: descriptor, maxCommandCount: 2, options: [])
        else { throw TestFailure("makeIndirectCommandBuffer returned nil for patch commands") }
        commands.indirectRenderCommandAt(0).drawPatches(
            4, patchStart: 0, patchCount: 2, patchIndexBuffer: nil, patchIndexBufferOffset: 0, instanceCount: 1,
            baseInstance: 0, tessellationFactorBuffer: factors, tessellationFactorBufferOffset: 0,
            tessellationFactorBufferInstanceStride: 0)
        commands.indirectRenderCommandAt(1).drawPatches(
            4, patchStart: 0, patchCount: 1, patchIndexBuffer: patchOrder, patchIndexBufferOffset: 0, instanceCount: 1,
            baseInstance: 0, tessellationFactorBuffer: factors, tessellationFactorBufferOffset: 0,
            tessellationFactorBufferInstanceStride: 0)
        return commands
    }
}
