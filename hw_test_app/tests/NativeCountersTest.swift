import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Counter sampling as the identity's AGX driver answers it: one counter set, "timestamp", of "GPUTimestamp", sampled
/// at stage boundaries alone, and its creation errors. One command buffer samples, in order, a render pass's four
/// boundaries, a compute and a blit encoder's two each, then a pass, two that load what the one before drew (passes
/// the raw path could fuse) and a parallel encoder's of two sub-encoders. A second command buffer resolves them with a
/// blit. Each boundary's start is at or before its end, every encoder's samples follow the one before it,
/// and every sample lies between the sampleTimestamps pairs taken before the commit and after the completion, at or
/// before GPUEndTime; the samples span no more than the GPU's time for the command buffer. The CPU's resolve and the
/// blit's agree word for word, unwritten samples read 0, and both command buffers stay raw.
struct NativeCountersTest: NativeObjectTest {
    let id = "metal.native.counters"
    let name = "Stage-boundary counter samples on the GPU's clock, resolved on the CPU and by a blit, on the raw path"

    static let samples = 28
    /// Each encoder's samples, in the order they run: render passes take four (vertex start and end, fragment start and
    /// end), the compute and the blit encoder two
    static let encoders = [0..<4, 4..<6, 6..<8, 8..<12, 12..<16, 16..<20, 20..<24]
    static let written = 24

    static func error(_ device: MTLDevice, _ configure: (MTLCounterSampleBufferDescriptor) -> Void) -> String {
        let descriptor = MTLCounterSampleBufferDescriptor()
        configure(descriptor)
        do {
            _ = try device.makeCounterSampleBuffer(descriptor: descriptor)
            return "made"
        } catch {
            let error = error as NSError
            return "\(error.domain == MTLCounterErrorDomain ? "counter" : error.domain)/\(error.code)"
        }
    }

    /// The device's counter set, its sampling points and its creation errors as the AGX driver answers them
    static func answers(_ device: MTLDevice, _ metrics: inout Metrics) throws -> (MTLCounterSet, [String]) {
        let sets = device.counterSets ?? []
        guard sets.count == 1, let set = sets.first, set.name == MTLCommonCounterSet.timestamp.rawValue,
            set.counters.map(\.name) == [MTLCommonCounter.timestamp.rawValue]
        else { throw TestFailure("counterSets is not the one timestamp set: \(sets.map(\.name))") }
        var failures: [String] = []
        let points: [MTLCounterSamplingPoint] = [
            .atStageBoundary, .atDrawBoundary, .atDispatchBoundary, .atTileDispatchBoundary, .atBlitBoundary,
        ]
        let sampled = points.map(device.supportsCounterSampling)
        if sampled != [true, false, false, false, false] { failures.append("sampling points \(sampled)") }
        let errors = [
            error(device) { $0.sampleCount = 4 },
            error(device) {
                $0.counterSet = set
                $0.sampleCount = 0
            },
            error(device) {
                $0.counterSet = set
                $0.sampleCount = 4097
            },
        ]
        metrics.set("errors", errors.joined(separator: ","))
        if errors != ["counter/1", "counter/1", "counter/1"] { failures.append("creation errors \(errors)") }
        return (set, failures)
    }

    static func pass(
        _ target: MTLTexture, _ counters: MTLCounterSampleBuffer, _ first: Int, load: MTLLoadAction
    )
        -> MTLRenderPassDescriptor
    {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = load
        pass.colorAttachments[0].storeAction = .store
        let samples = pass.sampleBufferAttachments[0]!
        samples.sampleBuffer = counters
        samples.startOfVertexSampleIndex = first
        samples.endOfVertexSampleIndex = first + 1
        samples.startOfFragmentSampleIndex = first + 2
        samples.endOfFragmentSampleIndex = first + 3
        return pass
    }

    static func words(_ data: Data?) -> [UInt64] {
        guard let data else { return [] }
        return data.withUnsafeBytes { Array($0.bindMemory(to: UInt64.self)) }
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        let (set, failures) = try Self.answers(device, &metrics)
        work.failures += failures
        let descriptor = MTLCounterSampleBufferDescriptor()
        descriptor.counterSet = set
        descriptor.sampleCount = Self.samples
        descriptor.storageMode = .shared
        let counters = try device.makeCounterSampleBuffer(descriptor: descriptor)
        work.objects.append(counters)

        let target = try gpu.texture(.rgba8Unorm, 32, 32)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        let kernel = try gpu.compute("fma_loop")
        let data = try gpu.buffer([SIMD4<Float>](repeating: SIMD4(1, 2, 3, 4), count: 256))
        let source = try gpu.buffer(4096), copy = try gpu.buffer(4096)
        func draw(_ encoder: MTLRenderCommandEncoder) {
            encoder.setRenderPipelineState(pipeline)
            GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(0, 1, 0, 1))
            encoder.endEncoding()
        }
        let before = device.sampleTimestamps()
        let sampling = try gpu.commit { commands in
            draw(commands.makeRenderCommandEncoder(descriptor: Self.pass(target, counters, 0, load: .clear))!)
            let computePass = MTLComputePassDescriptor()
            computePass.sampleBufferAttachments[0].sampleBuffer = counters
            computePass.sampleBufferAttachments[0].startOfEncoderSampleIndex = 4
            computePass.sampleBufferAttachments[0].endOfEncoderSampleIndex = 5
            let compute = commands.makeComputeCommandEncoder(descriptor: computePass)!
            compute.setComputePipelineState(kernel)
            compute.setBuffer(data, offset: 0, index: 0)
            var iterations: UInt32 = 256
            compute.setBytes(&iterations, length: 4, index: 1)
            compute.dispatchThreadgroups(
                MTLSize(width: 4, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
            compute.endEncoding()
            let blitPass = MTLBlitPassDescriptor()
            blitPass.sampleBufferAttachments[0].sampleBuffer = counters
            blitPass.sampleBufferAttachments[0].startOfEncoderSampleIndex = 6
            blitPass.sampleBufferAttachments[0].endOfEncoderSampleIndex = 7
            let blit = commands.makeBlitCommandEncoder(descriptor: blitPass)!
            blit.copy(from: source, sourceOffset: 0, to: copy, destinationOffset: 0, size: 4096)
            blit.endEncoding()
            draw(commands.makeRenderCommandEncoder(descriptor: Self.pass(target, counters, 8, load: .clear))!)
            draw(commands.makeRenderCommandEncoder(descriptor: Self.pass(target, counters, 12, load: .load))!)
            draw(commands.makeRenderCommandEncoder(descriptor: Self.pass(target, counters, 16, load: .load))!)
            let parallel = commands.makeParallelRenderCommandEncoder(
                descriptor: Self.pass(target, counters, 20, load: .load))!
            for _ in 0..<2 { draw(parallel.makeRenderCommandEncoder()!) }
            parallel.endEncoding()
        }
        try gpu.finish([sampling])
        let after = device.sampleTimestamps()
        work.paths.append(RawFacade.path(of: sampling))
        if before.cpu != before.gpu || after.cpu != after.gpu {
            work.failures.append("sampleTimestamps answered two clocks")
        }

        let cpu = try Self.words(counters.resolveCounterRange(0..<Self.samples))
        guard cpu.count == Self.samples else { throw TestFailure("resolveCounterRange gave \(cpu.count) samples") }
        let outside = [10..<(Self.samples + 1), 0..<0].compactMap { (try? counters.resolveCounterRange($0)) ?? nil }
        if !outside.isEmpty { work.failures.append("a resolve past the samples or of none answered data") }
        let resolved = try gpu.buffer(Self.samples * 8)
        let resolving = try gpu.commit { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.resolveCounters(counters, range: 0..<Self.samples, destinationBuffer: resolved, destinationOffset: 0)
            blit.endEncoding()
        }
        try gpu.finish([resolving])
        work.paths.append(RawFacade.path(of: resolving))
        let blitted = Array(
            UnsafeBufferPointer(
                start: resolved.contents().bindMemory(to: UInt64.self, capacity: Self.samples), count: Self.samples))
        if blitted != cpu {
            let differ = zip(blitted, cpu).filter { $0 != $1 }.count
            work.failures.append("the blit's resolve differs from the CPU's in \(differ) samples")
        }
        work.failures += Self.judge(cpu, sampling, before.gpu, after.gpu, &metrics)
        return work
    }

    /// What the samples say about the GPU's work: each was taken, in its encoder's order, inside the command buffer
    static func judge(
        _ cpu: [UInt64], _ sampling: MTLCommandBuffer, _ gpu0: UInt64, _ gpu1: UInt64, _ metrics: inout Metrics
    ) -> [String] {
        var failures: [String] = []
        let taken = Array(cpu[0..<written])
        if taken.contains(0) { failures.append("a boundary was not sampled: \(taken)") }
        if cpu[written...].contains(where: { $0 != 0 }) { failures.append("an unsampled index reads nonzero") }
        for range in encoders {
            for start in stride(from: range.lowerBound, to: range.upperBound, by: 2) where cpu[start] > cpu[start + 1] {
                failures.append("samples \(start) and \(start + 1): a start after its end")
            }
        }
        for (before, after) in zip(encoders, encoders.dropFirst()) where taken[before].max()! > taken[after].min()! {
            failures.append("samples \(after) begin before \(before) ended")
        }
        let first = taken.min()!, last = taken.max()!
        if first < gpu0 || last > gpu1 { failures.append("a sample outside the sampleTimestamps pairs") }
        let endNs = UInt64(sampling.gpuEndTime * 1e9), spanNs = (sampling.gpuEndTime - sampling.gpuStartTime) * 1e9
        if last > endNs + 1000 { failures.append("a sample after GPUEndTime by \(last - endNs) ns") }
        if RawFacade.gpuClock(of: sampling) == "ns" && Double(last - first) > spanNs + 1000 {
            failures.append("the samples span \(last - first) ns of a GPU time of \(Int(spanNs)) ns")
        }
        func us(_ later: UInt64, _ earlier: UInt64) -> Double { (Double(later) - Double(earlier)) / 1000 }
        metrics.set("render_us", us(cpu[3], cpu[0]), digits: 1)
        metrics.set("compute_us", us(cpu[5], cpu[4]), digits: 1)
        metrics.set("blit_us", us(cpu[7], cpu[6]), digits: 1)
        metrics.set("parallel_us", us(cpu[23], cpu[20]), digits: 1)
        metrics.set("samples_us", us(last, first), digits: 1)
        metrics.set("gpu_us", spanNs / 1000, digits: 1)
        metrics.set("lead_us", us(first, gpu0), digits: 1)
        metrics.set("tail_us", us(gpu1, last), digits: 1)
        metrics.set("clock", RawFacade.gpuClock(of: sampling) ?? "-")
        return failures
    }
}
