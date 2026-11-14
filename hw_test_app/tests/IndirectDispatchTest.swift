import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// An indirect dispatch on the raw path (boot 21 metal.native.shared_event: dispatchThreadgroupsWithIndirectBuffer: was
/// recorded as an invocation, call.inv, and its command buffer went to m2v). A kernel writes the threadgroup counts into
/// a buffer and the dispatch after it in the same encoder takes its grid from there: every thread of the grid writes its
/// index once and its threadgroups_per_grid, nothing past the grid is written, and a grid of no threadgroups writes
/// nothing. A stage-in kernel then takes its region from words a kernel wrote (setStageInRegionWithIndirectBuffer:).
struct IndirectDispatchTest: NativeObjectTest {
    let id = "gpu.raw.indirect_dispatch"
    let name = "Indirect dispatch grids and stage-in regions the GPU wrote, on the raw path"

    static let groups: SIMD3<UInt32> = [3, 2, 2]
    static let threads = MTLSize(width: 4, height: 2, depth: 1)
    static let capacity = 256

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        kernel void indirect_arguments(device uint *out [[buffer(0)]], constant uint4 &v [[buffer(1)]]) {
            out[0] = v.x; out[1] = v.y; out[2] = v.z;
        }
        kernel void indirect_grid(device uint *out [[buffer(0)]], device uint4 *seen [[buffer(1)]],
                                  uint3 p [[thread_position_in_grid]], uint3 n [[threads_per_grid]],
                                  uint3 g [[threadgroups_per_grid]]) {
            const uint i = p.x + n.x * (p.y + n.y * p.z);
            out[i] = i + 1;
            if (i == 0) seen[0] = uint4(g, 1);
        }
        kernel void indirect_region(device uint *out [[buffer(0)]], constant uint *v [[buffer(1)]]) {
            for (uint k = 0; k < 6; k++) out[k] = v[k];
        }
        struct In { float value [[attribute(0)]]; };
        kernel void indirect_stage_in(In in [[stage_in]], device float *out [[buffer(0)]],
                                      uint i [[thread_position_in_grid]]) {
            out[i] = in.value;
        }
        """

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        func kernel(_ name: String) throws -> MTLComputePipelineState {
            guard let f = library.makeFunction(name: name) else { throw TestFailure("no \(name)") }
            return try device.makeComputePipelineState(function: f)
        }
        let writer = try kernel("indirect_arguments"), grid = try kernel("indirect_grid")
        work.objects += [writer, grid]
        for (label, counts) in [("grid", Self.groups), ("empty", SIMD3<UInt32>(0, 1, 1))] {
            let arguments = try gpu.buffer([UInt32](repeating: 0, count: 4))
            let out = try gpu.buffer([UInt32](repeating: 0, count: Self.capacity))
            let seen = try gpu.buffer([UInt32](repeating: 0, count: 4))
            let timing = try await gpu.run { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(writer)
                encoder.setBuffer(arguments, offset: 0, index: 0)
                var v = SIMD4<UInt32>(counts.x, counts.y, counts.z, 0)
                encoder.setBytes(&v, length: 16, index: 1)
                encoder.dispatchThreadgroups(
                    MTLSize(width: 1, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
                encoder.setComputePipelineState(grid)
                encoder.setBuffer(out, offset: 0, index: 0)
                encoder.setBuffer(seen, offset: 0, index: 1)
                encoder.dispatchThreadgroups(
                    indirectBuffer: arguments, indirectBufferOffset: 0, threadsPerThreadgroup: Self.threads)
                encoder.endEncoding()
            }
            work.paths.append(timing.path)
            let total = Int(counts.x * counts.y * counts.z) * Self.threads.width * Self.threads.height
            let words = out.contents().bindMemory(to: UInt32.self, capacity: Self.capacity)
            let wrong = (0..<Self.capacity).filter { words[$0] != ($0 < total ? UInt32($0 + 1) : 0) }.count
            let g = seen.contents().bindMemory(to: UInt32.self, capacity: 4)
            metrics.set("\(label)_wrong", wrong)
            metrics.set("\(label)_threadgroups_per_grid", "\(g[0])/\(g[1])/\(g[2])")
            if wrong != 0 { work.failures.append("\(label): \(wrong) of \(Self.capacity) words wrong") }
            if total > 0 && (g[0] != counts.x || g[1] != counts.y || g[2] != counts.z || g[3] != 1) {
                work.failures.append("\(label): threadgroups_per_grid \(g[0])/\(g[1])/\(g[2])")
            }
        }
        try await stageIn(gpu, library, &metrics, &work)
        return work
    }

    /// A stage-in kernel over elements [5, 5 + 16) of its input, the region a kernel wrote
    func stageIn(_ gpu: GPU, _ library: MTLLibrary, _ metrics: inout Metrics, _ work: inout NativeWork) async throws {
        let device = gpu.device
        guard let f = library.makeFunction(name: "indirect_stage_in"),
            let w = library.makeFunction(name: "indirect_region")
        else { throw TestFailure("no stage-in functions") }
        let d = MTLComputePipelineDescriptor()
        d.computeFunction = f
        let input = MTLStageInputOutputDescriptor()
        input.attributes[0].format = .float
        input.attributes[0].bufferIndex = 1
        input.layouts[1].stride = 4
        input.layouts[1].stepFunction = .threadPositionInGridX
        d.stageInputDescriptor = input
        let make = { () throws -> (MTLComputePipelineState, MTLComputePipelineState) in
            (
                try device.makeComputePipelineState(descriptor: d, options: []).0,
                try device.makeComputePipelineState(function: w)
            )
        }
        let (state, writer) = try make()
        work.objects += [state, writer]
        let values = try gpu.buffer((0..<64).map { Float($0) * 2 + 1 })
        let region = try gpu.buffer([UInt32](repeating: 0, count: 6))
        let out = try gpu.buffer([Float](repeating: -1, count: 16))
        let timing = try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(writer)
            encoder.setBuffer(region, offset: 0, index: 0)
            var words: [UInt32] = [5, 0, 0, 16, 1, 1]
            encoder.setBytes(&words, length: 24, index: 1)
            encoder.dispatchThreadgroups(
                MTLSize(width: 1, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
            encoder.setComputePipelineState(state)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.setBuffer(values, offset: 0, index: 1)
            encoder.setStageInRegionWithIndirectBuffer(region, indirectBufferOffset: 0)
            encoder.dispatchThreadgroups(
                MTLSize(width: 16, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
            encoder.endEncoding()
        }
        work.paths.append(timing.path)
        let results = out.contents().bindMemory(to: Float.self, capacity: 16)
        let wrong = (0..<16).filter { results[$0] != Float(5 + $0) * 2 + 1 }.count
        metrics.set("stage_in_wrong", wrong)
        metrics.set("stage_in_first", "\(results[0])")
        if wrong != 0 { work.failures.append("stage_in: \(wrong) of 16 elements wrong") }
    }
}
