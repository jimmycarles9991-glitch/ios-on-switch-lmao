import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The bytes of RGBA32Float texels as floats
private func floats(_ bytes: [UInt8]) -> [Float] {
    bytes.withUnsafeBytes { Array($0.bindMemory(to: Float.self)) }
}

/// maxFramebufferStorageBits (NativeAnswers.framebufferBits): a pass of eight RGBA32Float targets, every bit of a pixel
/// the answer counts, each target written its own value at every texel by one draw, exact.
struct NativeWideTargetsTest: NativeObjectTest {
    let id = "metal.native.wide_targets"
    let name = "Eight RGBA32Float targets in one pass, the framebuffer bits the device answers, on the raw path"

    static let size = 16, targets = 8

    static func want(_ k: Int, _ x: Int, _ y: Int) -> [Float] {
        [Float(x + 1000 * k), Float(y), Float(k) + 0.5, -Float(k)]
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        if let wrong = NativeAnswers.check(
            gpu.device, "maxFramebufferStorageBits", NativeAnswers.framebufferBits, field: "framebuffer_bits", &metrics)
        {
            work.failures.append(wrong)
        }
        let pipeline = try gpu.render(
            vertex: "answers_full_vertex", fragment: "wide_targets_fragment", format: .rgba32Float
        ) { d in
            for k in 1..<Self.targets { d.colorAttachments[k].pixelFormat = .rgba32Float }
        }
        let textures = try (0..<Self.targets).map { _ in try gpu.texture(.rgba32Float, Self.size, Self.size) }
        work.objects += [pipeline] + textures
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            for (k, t) in textures.enumerated() {
                pass.colorAttachments[k].texture = t
                pass.colorAttachments[k].loadAction = .clear
                pass.colorAttachments[k].storeAction = .store
            }
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            e.endEncoding()
        }
        work.paths.append(timing.path)
        var wrong = 0
        for (k, t) in textures.enumerated() {
            let texels = floats(try await gpu.read(t, bytesPerPixel: 16))
            for y in 0..<Self.size {
                for x in 0..<Self.size
                where Array(texels[(y * Self.size + x) * 4..<(y * Self.size + x) * 4 + 4])
                    != Self.want(k, x, y)
                {
                    wrong += 1
                }
            }
        }
        metrics.set("wrong_texels", wrong)
        if wrong != 0 { work.failures.append("\(wrong) texels of the eight targets wrong") }
        return work
    }
}

/// maxInterpolatedComponents (NativeAnswers.interpolants): a vertex function writes as many float4 varyings as the answer
/// counts components (32 on 21H461, 31 on 24B119), every one interpolated and read by the fragment function, whose
/// weighted sum fills an RGBA32Float target.
struct NativeInterpolantsTest: NativeObjectTest {
    let id = "metal.native.interpolants"
    let name = "As many interpolated components as the device answers reach the fragment function, on the raw path"

    static let size = 8

    static func varying(_ i: Int) -> [Float] {
        [Float(i), Float(i) + 0.25, 0.5 * Float(i), 1 + Float(i)]
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        if let wrong = NativeAnswers.check(
            gpu.device, "maxInterpolatedComponents", NativeAnswers.interpolants, field: "interpolants", &metrics)
        {
            work.failures.append(wrong)
        }
        let count = NativeAnswers.interpolants / 4
        metrics.set("varyings", count)
        let pipeline = try gpu.render(
            vertex: "interpolants\(count)_vertex", fragment: "interpolants\(count)_fragment", format: .rgba32Float)
        let target = try gpu.texture(.rgba32Float, Self.size, Self.size)
        work.objects += [pipeline, target]
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            e.endEncoding()
        }
        work.paths.append(timing.path)
        let want = (0..<4).map { c in (0..<count).reduce(Float(0)) { $0 + Self.varying($1)[c] * Float($1 + 1) } }
        let texels = floats(try await gpu.read(target, bytesPerPixel: 16))
        let wrong = (0..<Self.size * Self.size).filter { i in
            (0..<4).contains { c in abs(texels[i * 4 + c] - want[c]) > 0.05 }
        }.count
        metrics.set("wrong_texels", wrong)
        if wrong != 0 { work.failures.append("\(wrong) texels miss a varying") }
        return work
    }
}

/// maxThreadgroupMemoryLength (NativeAnswers.threadgroupMemory, GM20B's 48 KiB a CTA): one threadgroup of 256 threads
/// over that much threadgroup memory, every word written by one thread and read back by the next after the barrier, so
/// the words past 32 KiB cross threads too.
struct NativeThreadgroupMemoryTest: NativeObjectTest {
    let id = "metal.native.threadgroup_memory"
    let name = "A kernel uses all the threadgroup memory the device answers, on the raw path"

    static let threads = 256

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        let bytes = NativeAnswers.answer(gpu.device, "maxThreadgroupMemoryLength") ?? 0
        metrics.set("threadgroup_bytes", bytes)
        if bytes != NativeAnswers.threadgroupMemory {
            work.failures.append(
                "maxThreadgroupMemoryLength answers \(bytes), the table \(NativeAnswers.threadgroupMemory)")
        }
        if let wrong = NativeAnswers.check(
            gpu.device, "maxComputeThreadgroupMemory", NativeAnswers.threadgroupMemory,
            field: "compute_threadgroup_bytes",
            &metrics)
        {
            work.failures.append(wrong)
        }
        guard bytes >= 4 else { return work }
        let state = try gpu.compute("threadgroup_span")
        let words = UInt32(bytes / 4)
        let out = try gpu.buffer(4 * Self.threads)
        work.objects.append(state)
        let timing = try await gpu.run { commands in
            let e = commands.makeComputeCommandEncoder()!
            e.setComputePipelineState(state)
            e.setThreadgroupMemoryLength(bytes, index: 0)
            e.setBuffer(out, offset: 0, index: 0)
            var count = words
            e.setBytes(&count, length: 4, index: 1)
            e.dispatchThreadgroups(
                MTLSize(width: 1, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: Self.threads, height: 1, depth: 1))
            e.endEncoding()
        }
        work.paths.append(timing.path)
        let got = out.contents().bindMemory(to: UInt32.self, capacity: Self.threads)
        let wrong = (0..<Self.threads).filter { tid in
            var x: UInt32 = 0
            var i = UInt32((tid + 1) % Self.threads)
            while i < words {
                x ^= (i &* 2_654_435_761) &+ i
                i += UInt32(Self.threads)
            }
            return got[tid] != x
        }.count
        metrics.set("wrong_threads", wrong)
        if wrong != 0 { work.failures.append("\(wrong) threads read words other threads did not write") }
        return work
    }
}
