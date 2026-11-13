import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Compute stage-in: a kernel whose [[stage_in]] float2 comes from buffer 1 per thread position, over the dispatch's
/// whole grid as its stage-in region, writes x + y of each element. A static layout of stride 16 runs on both kinds; on
/// 24B119's A13 a layout of MTLBufferLayoutStrideDynamic runs at the bind's attributeStride, 16 and then 24 (21H461's
/// A10X answers supportsDynamicAttributeStride NO and refuses that state). Each runs again as an indirect command buffer's
/// dispatch command that sets its own pipeline, buffers (the per-bind stride by setKernelBuffer:offset:attributeStride:)
/// and stage-in region. Each result is compared with the CPU's; every command buffer stays raw.
struct NativeStageInTest: NativeObjectTest {
    let id = "metal.native.stage_in"
    let name = "Compute stage-in from static and per-bind strides on the raw path"

    static let count = 64

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct In { float2 value [[attribute(0)]]; };
        kernel void stage_sum(In in [[stage_in]], device float *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {
            out[i] = in.value.x + in.value.y;
        }
        """

    /// Element k's (k, 2k + 1) `stride` bytes apart, junk between
    static func elements(_ stride: Int) -> [Float] {
        let floats = stride / 4
        var out = [Float](repeating: -1e6, count: floats * (count + 1))
        for k in 0...count {
            out[k * floats] = Float(k)
            out[k * floats + 1] = Float(2 * k + 1)
        }
        return out
    }

    static func descriptor(_ function: MTLFunction, stride: Int) -> MTLComputePipelineDescriptor {
        let d = MTLComputePipelineDescriptor()
        d.computeFunction = function
        d.supportIndirectCommandBuffers = true
        let input = MTLStageInputOutputDescriptor()
        input.attributes[0].format = .float2
        input.attributes[0].bufferIndex = 1
        input.layouts[1].stride = stride
        input.layouts[1].stepFunction = .threadPositionInGridX
        d.stageInputDescriptor = input
        return d
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        guard let function = library.makeFunction(name: "stage_sum") else { throw TestFailure("no stage_sum") }
        let make = { (d: MTLComputePipelineDescriptor) throws -> MTLComputePipelineState in
            try device.makeComputePipelineState(descriptor: d, options: []).0
        }
        let dynamicStride = Int(bitPattern: UInt.max)  // MTLBufferLayoutStrideDynamic
        let dynamic =
            (device as AnyObject as! NSObject).value(forKey: "supportsDynamicAttributeStride") as? Bool ?? false
        metrics.set("dynamic_stride", dynamic ? "yes" : "no")
        var cases = [("static16", try make(Self.descriptor(function, stride: 16)), 16, false)]
        if dynamic {
            let state = try make(Self.descriptor(function, stride: dynamicStride))
            cases += [("dynamic16", state, 16, true), ("dynamic24", state, 24, true)]
        } else if (try? make(Self.descriptor(function, stride: dynamicStride))) != nil {
            work.failures.append("a dynamic-stride stage-in state was made where the device answers it NO")
        }
        let groups = MTLSize(width: Self.count + 1, height: 1, depth: 1)
        let one = MTLSize(width: 1, height: 1, depth: 1)
        for (label, state, stride, perBind) in cases {
            work.objects.append(state)
            for indirect in [false, true] {
                let input = try gpu.buffer(Self.elements(stride))
                let output = try gpu.buffer([Float](repeating: 0, count: Self.count + 1))
                let commands = indirect ? try Self.command(device, state, input, output, stride, perBind) : nil
                if let commands { work.objects.append(commands) }
                let timing = try await gpu.run { buffer in
                    let encoder = buffer.makeComputeCommandEncoder()!
                    if let commands {
                        encoder.useResource(input, usage: .read)
                        encoder.useResource(output, usage: .write)
                        encoder.executeCommandsInBuffer(commands, range: 0..<1)
                    } else {
                        encoder.setComputePipelineState(state)
                        encoder.setBuffer(output, offset: 0, index: 0)
                        if perBind {
                            encoder.setBuffer(input, offset: 0, attributeStride: stride, index: 1)
                        } else {
                            encoder.setBuffer(input, offset: 0, index: 1)
                        }
                        encoder.setStageInRegion(MTLRegionMake1D(0, Self.count + 1))
                        encoder.dispatchThreadgroups(groups, threadsPerThreadgroup: one)
                    }
                    encoder.endEncoding()
                }
                work.paths.append(timing.path)
                let results = output.contents().bindMemory(to: Float.self, capacity: Self.count + 1)
                let wrong = (0...Self.count).filter { results[$0] != Float(3 * $0 + 1) }.count
                let name = indirect ? "icb_\(label)" : label
                metrics.set("\(name)_wrong", wrong)
                if wrong != 0 { work.failures.append("\(name): \(wrong) of \(Self.count + 1) elements wrong") }
            }
        }
        return work
    }

    /// An indirect command buffer of one dispatch command of `state` over the whole input, its buffers and stage-in
    /// region its own
    static func command(
        _ device: MTLDevice, _ state: MTLComputePipelineState, _ input: MTLBuffer, _ output: MTLBuffer, _ stride: Int,
        _ perBind: Bool
    ) throws -> MTLIndirectCommandBuffer {
        let descriptor = MTLIndirectCommandBufferDescriptor()
        descriptor.commandTypes = [.concurrentDispatch]
        descriptor.inheritPipelineState = false
        descriptor.inheritBuffers = false
        descriptor.maxKernelBufferBindCount = 2
        guard let commands = device.makeIndirectCommandBuffer(descriptor: descriptor, maxCommandCount: 1, options: [])
        else { throw TestFailure("makeIndirectCommandBuffer returned nil for a dispatch command") }
        let command = commands.indirectComputeCommandAt(0)
        command.setComputePipelineState(state)
        command.setKernelBuffer(output, offset: 0, at: 0)
        if perBind {
            command.setKernelBuffer(input, offset: 0, attributeStride: stride, at: 1)
        } else {
            command.setKernelBuffer(input, offset: 0, at: 1)
        }
        command.setStageInRegion(MTLRegionMake1D(0, count + 1))
        command.concurrentDispatchThreadgroups(
            MTLSize(width: count + 1, height: 1, depth: 1),
            threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
        return commands
    }
}
