import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A compute state the pack holds no answers of (icb_fold at a thread limit of 192, which no corpus line names) gets its
/// maxTotalThreadsPerThreadgroup, threadExecutionWidth and staticThreadgroupMemoryLength from the system shader compiler
/// at its creation (drivers/TegraGM20BMetal/raw/runtime/RawMetalComputeState.mm compilerAnswers), never from m2v, and
/// its dispatch runs on the raw path. So does tags_kernel made with a stage input, whose facts request carries it. The case
/// fails if either creation asked for the m2v device (m2v_needs_asked).
struct ComputeFactsTest: HWTest {
    let id = "gpu.raw.compute_facts"
    let name = "A compute state the pack lacks, answered by the shader compiler"
    let subsystem = Subsystem.metal

    static let count = 256

    /// tags_kernel (shaders/FunctionTags.metal) made with a stage input: its facts travel with the stage input to the
    /// shader compiler, which keys them as the pack does
    static func stageInputState(_ gpu: GPU) async throws -> MTLComputePipelineState {
        let values = MTLFunctionConstantValues()
        var mode: Int32 = 2
        values.setConstantValue(&mode, type: .int, index: 3)
        let descriptor = MTLComputePipelineDescriptor()
        descriptor.computeFunction = try await gpu.library.makeFunction(name: "tags_kernel", constantValues: values)
        descriptor.stageInputDescriptor = MTLStageInputOutputDescriptor()
        descriptor.stageInputDescriptor?.attributes[1].format = .float2
        descriptor.stageInputDescriptor?.attributes[1].bufferIndex = 0
        descriptor.stageInputDescriptor?.attributes[4].format = .uint
        descriptor.stageInputDescriptor?.attributes[4].offset = 8
        descriptor.stageInputDescriptor?.attributes[4].bufferIndex = 0
        descriptor.stageInputDescriptor?.layouts[0].stride = 12
        return try gpu.device.makeComputePipelineState(descriptor: descriptor, options: [], reflection: nil)
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let before = OwnAnswersTest.needs(gpu.device)
        guard let function = gpu.library.makeFunction(name: "icb_fold") else { throw TestFailure("no icb_fold") }
        let descriptor = MTLComputePipelineDescriptor()
        descriptor.computeFunction = function
        descriptor.maxTotalThreadsPerThreadgroup = 192
        let started = Date()
        let state = try gpu.device.makeComputePipelineState(descriptor: descriptor, options: [], reflection: nil)
        let createdMs = Date().timeIntervalSince(started) * 1000
        let staged = try await Self.stageInputState(gpu)
        let asked = OwnAnswersTest.needs(gpu.device).filter { $0.value > (before[$0.key] ?? 0) }.keys.sorted()

        let data = try gpu.buffer(Self.count * 4)
        let words = data.contents().bindMemory(to: UInt32.self, capacity: Self.count)
        for i in 0..<Self.count { words[i] = UInt32(i) }
        var value: UInt32 = 5
        let width = min(state.maxTotalThreadsPerThreadgroup, 64)
        let path = try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(state)
            encoder.setBuffer(data, offset: 0, index: 0)
            encoder.setBytes(&value, length: 4, index: 1)
            encoder.dispatchThreads(
                MTLSize(width: Self.count, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: width, height: 1, depth: 1))
            encoder.endEncoding()
        }.path
        var off = 0
        for i in 0..<Self.count where words[i] != UInt32(i) * 3 + 5 { off += 1 }

        var metrics = Metrics()
        metrics.set("max_threads", state.maxTotalThreadsPerThreadgroup)
        metrics.set("execution_width", state.threadExecutionWidth)
        metrics.set("static_memory", state.staticThreadgroupMemoryLength)
        metrics.set("created_ms", createdMs)
        metrics.set("words_off", off)
        metrics.set("path", path)
        metrics.set("stage_input_max_threads", staged.maxTotalThreadsPerThreadgroup)
        metrics.set("m2v_needs_asked", asked.isEmpty ? "-" : asked.joined(separator: ","))
        var wrong: [String] = []
        if state.maxTotalThreadsPerThreadgroup < 1 || state.maxTotalThreadsPerThreadgroup > 192 { wrong.append("max") }
        if state.threadExecutionWidth != 32 { wrong.append("width") }
        if staged.maxTotalThreadsPerThreadgroup < 1 || staged.threadExecutionWidth != 32 { wrong.append("stage_input") }
        if off > 0 { wrong.append("words") }
        if path != "raw" { wrong.append("path") }
        if !asked.isEmpty { wrong.append("m2v") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
