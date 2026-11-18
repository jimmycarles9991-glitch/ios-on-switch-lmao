import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Indirect command buffers of dispatches (icb_fold: each element becomes element * 3 + value, so order shows), compared
/// with the CPU's fold of the same commands. A buffer of four commands that set their own pipeline and buffers, encoded
/// once on the CPU: dispatchThreads over a grid that is not a multiple of its threadgroup (two regions) into A, then behind
/// a barrier threadgroups into A again, threadgroups into B beside them, and a fourth reset to nothing. Executed whole, then
/// the third alone, then by a range {0, 1} in a private buffer a blit copy wrote in the same command buffer (the GPU reads
/// it, the CPU never does). A buffer that inherits the encoder's buffers and one that inherits its pipeline run under the
/// encoder's state, then each twice in one encoder: the first under two sets of buffers (the GPU writes its descriptors
/// at each execute), the second folding twice (its commands made once for the pipeline). Last, a blit resets the first
/// command and copies the second (its barrier with it) over the fourth on the GPU, and an execute after it in the same
/// command buffer runs what the GPU left. Every command buffer stays raw.
struct NativeComputeCommandsTest: NativeObjectTest {
    let id = "metal.native.compute_commands"
    let name = "Indirect command buffers of dispatches: encoded once, executed by range on the raw path"

    static let count = 100

    /// What the commands leave in an element starting from 0: each fold in order
    static func folded(_ values: [UInt32]) -> UInt32 { values.reduce(0) { $0 &* 3 &+ $1 } }

    static func wrong(_ buffer: MTLBuffer, _ want: UInt32) -> Int {
        let words = buffer.contents().bindMemory(to: UInt32.self, capacity: count)
        return (0..<count).filter { words[$0] != want }.count
    }

    static func zero(_ buffers: [MTLBuffer]) {
        for b in buffers { memset(b.contents(), 0, b.length) }
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        guard let function = gpu.library.makeFunction(name: "icb_fold") else { throw TestFailure("no icb_fold") }
        let state = MTLComputePipelineDescriptor()
        state.computeFunction = function
        state.supportIndirectCommandBuffers = true
        let pipeline = try device.makeComputePipelineState(descriptor: state, options: [], reflection: nil)
        let a = try gpu.buffer(Self.count * 4), b = try gpu.buffer(Self.count * 4), c = try gpu.buffer(Self.count * 4)
        let one = try gpu.buffer([UInt32](arrayLiteral: 1)), two = try gpu.buffer([UInt32](arrayLiteral: 2))
        let three = try gpu.buffer([UInt32](arrayLiteral: 3))

        let own = MTLIndirectCommandBufferDescriptor()
        own.commandTypes = [.concurrentDispatch, .concurrentDispatchThreads]
        own.inheritPipelineState = false
        own.inheritBuffers = false
        own.maxKernelBufferBindCount = 2
        guard let commands = device.makeIndirectCommandBuffer(descriptor: own, maxCommandCount: 4, options: []) else {
            throw TestFailure("makeIndirectCommandBuffer returned nil for dispatches")
        }
        var work = NativeWork(objects: [commands, pipeline])
        let encodes: [(MTLBuffer, MTLBuffer)] = [(a, one), (a, two), (b, three), (c, one)]
        for (i, (data, value)) in encodes.enumerated() {
            let command = commands.indirectComputeCommandAt(i)
            command.setComputePipelineState(pipeline)
            command.setKernelBuffer(data, offset: 0, at: 0)
            command.setKernelBuffer(value, offset: 0, at: 1)
            switch i {
            case 0:
                command.concurrentDispatchThreads(
                    MTLSize(width: Self.count, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 32, height: 1, depth: 1))
            case 1:
                command.setBarrier()
                command.concurrentDispatchThreadgroups(
                    MTLSize(width: 4, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 25, height: 1, depth: 1))
            default:
                command.concurrentDispatchThreadgroups(
                    MTLSize(width: 1, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: Self.count, height: 1, depth: 1))
            }
        }
        commands.indirectComputeCommandAt(3).reset()

        func check(_ name: String, _ want: [(MTLBuffer, UInt32)]) {
            let wrong = want.map { Self.wrong($0.0, $0.1) }.reduce(0, +)
            metrics.set("wrong_\(name)", wrong)
            if wrong != 0 { work.failures.append("\(wrong) elements wrong after \(name)") }
        }

        for (name, range, want) in [
            ("whole", 0..<4, [(a, Self.folded([1, 2])), (b, Self.folded([3])), (c, UInt32(0))]),
            ("third", 2..<3, [(a, UInt32(0)), (b, Self.folded([3])), (c, UInt32(0))]),
        ] {
            Self.zero([a, b, c])
            let timing = try await gpu.run { buffer in
                let encoder = buffer.makeComputeCommandEncoder()!
                encoder.useResources([a, b, c, one, two, three], usage: [.read, .write])
                encoder.executeCommandsInBuffer(commands, range: range)
                encoder.endEncoding()
            }
            work.paths.append(timing.path)
            check(name, want)
        }

        Self.zero([a, b, c])
        let staged = try gpu.buffer([UInt32](arrayLiteral: 0, 1))
        guard let rangeBuffer = device.makeBuffer(length: 8, options: .storageModePrivate) else {
            throw TestFailure("no private range buffer")
        }
        let gpuRange = try await gpu.run { buffer in
            let blit = buffer.makeBlitCommandEncoder()!
            blit.copy(from: staged, sourceOffset: 0, to: rangeBuffer, destinationOffset: 0, size: 8)
            blit.endEncoding()
            let encoder = buffer.makeComputeCommandEncoder()!
            encoder.executeCommandsInBuffer(commands, indirectBuffer: rangeBuffer, offset: 0)
            encoder.endEncoding()
        }
        work.paths.append(gpuRange.path)
        check("gpu_range", [(a, Self.folded([1])), (b, UInt32(0))])

        let inheritsBuffers = MTLIndirectCommandBufferDescriptor()
        inheritsBuffers.commandTypes = [.concurrentDispatch]
        inheritsBuffers.inheritPipelineState = false
        inheritsBuffers.inheritBuffers = true
        let inheritsPipeline = MTLIndirectCommandBufferDescriptor()
        inheritsPipeline.commandTypes = [.concurrentDispatch]
        inheritsPipeline.inheritPipelineState = true
        inheritsPipeline.inheritBuffers = false
        inheritsPipeline.maxKernelBufferBindCount = 2
        guard
            let buffers = device.makeIndirectCommandBuffer(
                descriptor: inheritsBuffers, maxCommandCount: 1, options: []),
            let pipelines = device.makeIndirectCommandBuffer(
                descriptor: inheritsPipeline, maxCommandCount: 1, options: [])
        else { throw TestFailure("makeIndirectCommandBuffer returned nil for inherited state") }
        work.objects += [buffers, pipelines]
        let group = MTLSize(width: 1, height: 1, depth: 1), threads = MTLSize(width: Self.count, height: 1, depth: 1)
        buffers.indirectComputeCommandAt(0).setComputePipelineState(pipeline)
        buffers.indirectComputeCommandAt(0).concurrentDispatchThreadgroups(group, threadsPerThreadgroup: threads)
        pipelines.indirectComputeCommandAt(0).setKernelBuffer(b, offset: 0, at: 0)
        pipelines.indirectComputeCommandAt(0).setKernelBuffer(three, offset: 0, at: 1)
        pipelines.indirectComputeCommandAt(0).concurrentDispatchThreadgroups(group, threadsPerThreadgroup: threads)
        Self.zero([a, b, c])
        let inherited = try await gpu.run { buffer in
            let encoder = buffer.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setBuffer(a, offset: 0, index: 0)
            encoder.setBuffer(two, offset: 0, index: 1)
            encoder.executeCommandsInBuffer(buffers, range: 0..<1)
            encoder.memoryBarrier(scope: .buffers)
            encoder.executeCommandsInBuffer(pipelines, range: 0..<1)
            encoder.endEncoding()
        }
        work.paths.append(inherited.path)
        check("inherited", [(a, Self.folded([2])), (b, Self.folded([3]))])

        Self.zero([a, b, c])
        let offset = try gpu.buffer([UInt32](arrayLiteral: 0, 0, 0, 0, 2))
        let rebound = try await gpu.run { buffer in
            let encoder = buffer.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setBuffer(a, offset: 0, index: 0)
            encoder.setBuffer(one, offset: 0, index: 1)
            encoder.executeCommandsInBuffer(buffers, range: 0..<1)
            encoder.memoryBarrier(scope: .buffers)
            encoder.setBuffer(c, offset: 0, index: 0)
            encoder.setBuffer(offset, offset: 16, index: 1)
            encoder.executeCommandsInBuffer(buffers, range: 0..<1)
            encoder.memoryBarrier(scope: .buffers)
            encoder.executeCommandsInBuffer(pipelines, range: 0..<1)
            encoder.memoryBarrier(scope: .buffers)
            encoder.executeCommandsInBuffer(pipelines, range: 0..<1)
            encoder.endEncoding()
        }
        work.paths.append(rebound.path)
        check("rebound", [(a, Self.folded([1])), (c, Self.folded([2])), (b, Self.folded([3, 3]))])

        Self.zero([a, b, c])
        let edited = try await gpu.run { buffer in
            let blit = buffer.makeBlitCommandEncoder()!
            blit.resetCommandsInBuffer(commands, range: 0..<1)
            blit.copyIndirectCommandBuffer(commands, sourceRange: 1..<2, destination: commands, destinationIndex: 3)
            blit.endEncoding()
            let encoder = buffer.makeComputeCommandEncoder()!
            encoder.executeCommandsInBuffer(commands, range: 0..<4)
            encoder.endEncoding()
        }
        work.paths.append(edited.path)
        check("edited", [(a, Self.folded([2, 2])), (b, Self.folded([3])), (c, UInt32(0))])

        let patches = MTLIndirectCommandBufferDescriptor()
        patches.commandTypes = [.drawPatches]
        metrics.set(
            "patches",
            device.makeIndirectCommandBuffer(descriptor: patches, maxCommandCount: 1, options: []) == nil
                ? "nil" : "made")
        return work
    }
}
