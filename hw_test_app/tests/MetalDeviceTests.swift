import Foundation
import HWTestCore
import HWTestGPU
import Metal

struct MetalDeviceTest: HWTest {
    let id = "metal.device"
    let name = "Device, family and limits"
    let subsystem = Subsystem.metal

    static let appleFamilies: [(MTLGPUFamily, Int)] = [
        (.apple1, 1), (.apple2, 2), (.apple3, 3), (.apple4, 4), (.apple5, 5), (.apple6, 6), (.apple7, 7), (.apple8, 8),
        (.apple9, 9),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        guard let device = MTLCreateSystemDefaultDevice() else {
            return .fail("MTLCreateSystemDefaultDevice returned nil")
        }
        var metrics = Metrics()
        let family = Self.appleFamilies.last { device.supportsFamily($0.0) }?.1 ?? 0
        metrics.set("apple_family", family)
        metrics.set("common3", device.supportsFamily(.common3))
        metrics.set("metal3", device.supportsFamily(.metal3))
        metrics.set("threads_per_group", device.maxThreadsPerThreadgroup.width)
        metrics.set("threadgroup_kb", device.maxThreadgroupMemoryLength / 1024)
        metrics.set("max_buffer_mb", device.maxBufferLength >> 20)
        metrics.set("working_set_mb", Int(device.recommendedMaxWorkingSetSize >> 20))
        metrics.set("argument_buffers_tier", Int(device.argumentBuffersSupport.rawValue) + 1)
        metrics.set("rw_texture_tier", Int(device.readWriteTextureSupport.rawValue))
        metrics.set(
            "msaa", [2, 4, 8].filter { device.supportsTextureSampleCount($0) }.map(String.init).joined(separator: ","))
        metrics.set("bc", device.supportsBCTextureCompression)
        metrics.set("float32_filtering", device.supports32BitFloatFiltering)
        metrics.set("unified", device.hasUnifiedMemory)
        return .judged(
            [
                ("an Apple GPU family", family >= 1),
                ("a non-empty name", !device.name.isEmpty),
                ("recommendedMaxWorkingSetSize", device.recommendedMaxWorkingSetSize > 0),
                ("4x MSAA", device.supportsTextureSampleCount(4)),
            ], metrics, proof: "\(device.name) registry=\(String(device.registryID, radix: 16))")
    }
}

struct MetalComputeTest: HWTest {
    let id = "metal.compute"
    let name = "Compute kernel, checked result"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let count = 1 << 20
        let a = (0..<count).map { Float($0 % 977) * 0.5 }
        let b = (0..<count).map { Float($0 % 13) - 6 }
        let out = try gpu.buffer(count * 4)
        let counter = try gpu.buffer([UInt32(0)])
        let pipeline = try gpu.compute("multiply_add")
        let timing = try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setBuffer(try gpu.buffer(a), offset: 0, index: 0)
            encoder.setBuffer(try gpu.buffer(b), offset: 0, index: 1)
            encoder.setBuffer(out, offset: 0, index: 2)
            encoder.setBuffer(counter, offset: 0, index: 3)
            encoder.dispatchThreads(
                MTLSize(width: count, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(
                    width: min(256, pipeline.maxTotalThreadsPerThreadgroup), height: 1, depth: 1))
            encoder.endEncoding()
        }
        let results = out.contents().bindMemory(to: Float.self, capacity: count)
        // Every product and sum is a small multiple of 0.5, exact in float32, so fma and a*b+c agree bit for bit.
        let wrong = (0..<count).filter { results[$0] != a[$0] * b[$0] + Float($0 & 1023) }
        let atomics = counter.contents().load(as: UInt32.self)
        var metrics = Metrics()
        metrics.set("elements", count)
        metrics.set("wrong", wrong.count)
        metrics.set("atomic_count", Int(atomics))
        metrics.set("gpu_ms", timing.gpuMilliseconds, digits: 3)
        return .judged(
            [
                ("every element exact (first wrong \(wrong.first.map(String.init) ?? "-"))", wrong.isEmpty),
                ("one atomic increment per thread", atomics == UInt32(count)),
                ("GPU timestamps advance", timing.gpuTimed),
            ], metrics, proof: gpu.proof)
    }
}

struct MetalRuntimeCompileTest: HWTest {
    let id = "metal.runtime_compile"
    let name = "Shader compiled from source at run time"
    let subsystem = Subsystem.metal

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        kernel void square(device uint *data [[buffer(0)]], uint id [[thread_position_in_grid]]) {
            data[id] = id * id + 7;
        }
        """

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let start = CFAbsoluteTimeGetCurrent()
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        let compileMs = (CFAbsoluteTimeGetCurrent() - start) * 1000
        guard let function = library.makeFunction(name: "square") else { return .fail("no square function") }
        let pipeline = try await gpu.device.makeComputePipelineState(function: function)
        let count = 4096
        let data = try gpu.buffer(count * 4)
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setBuffer(data, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: count, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
            encoder.endEncoding()
        }
        let values = data.contents().bindMemory(to: UInt32.self, capacity: count)
        let wrong = (0..<count).filter { values[$0] != UInt32($0 * $0 + 7) }.count
        var metrics = Metrics()
        metrics.set("compile_ms", compileMs, digits: 1)
        metrics.set("wrong", wrong)
        return .judged([("every element exact", wrong == 0)], metrics, proof: gpu.proof)
    }
}

struct MetalBenchmarkTest: HWTest {
    let id = "metal.benchmark"
    let name = "Timed GPU benchmark (ALU, copy, fill)"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        var metrics = Metrics()

        let threads = 1 << 18
        var iterations: UInt32 = 512
        let data = try gpu.buffer((0..<threads * 4).map { Float($0 % 7) * 0.001 })
        let fma = try gpu.compute("fma_loop")
        let alu = try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(fma)
            encoder.setBuffer(data, offset: 0, index: 0)
            encoder.setBytes(&iterations, length: 4, index: 1)
            encoder.dispatchThreads(
                MTLSize(width: threads, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
            encoder.endEncoding()
        }
        // Two float4 FMAs per iteration: 16 floating-point operations.
        let flops = Double(threads) * Double(iterations) * 16
        metrics.set("gflops", alu.gpuTimed ? flops / alu.gpuMilliseconds / 1e6 : 0, digits: 1)

        let bytes = 64 << 20
        let source = try gpu.buffer(bytes)
        let destination = try gpu.buffer(bytes)
        let copy = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(from: source, sourceOffset: 0, to: destination, destinationOffset: 0, size: bytes)
            blit.endEncoding()
        }
        metrics.set("copy_gbps", copy.gpuTimed ? Double(2 * bytes) / copy.gpuMilliseconds / 1e6 : 0, digits: 2)

        let (width, height, draws) = (1920, 1080, 16)
        let target = try gpu.texture(.bgra8Unorm, width, height)
        let solid = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let fill = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(solid)
            for index in 0..<draws {
                GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(Float(index) / Float(draws), 0.5, 0.25, 1))
            }
            encoder.endEncoding()
        }
        metrics.set(
            "fill_gpix_s", fill.gpuTimed ? Double(width * height * draws) / fill.gpuMilliseconds / 1e6 : 0, digits: 2)
        metrics.set("alu_ms", alu.gpuMilliseconds, digits: 2)
        metrics.set("copy_ms", copy.gpuMilliseconds, digits: 2)
        metrics.set("fill_ms", fill.gpuMilliseconds, digits: 2)
        return .judged(
            [("GPU timestamps advance in all three passes", alu.gpuTimed && copy.gpuTimed && fill.gpuTimed)], metrics,
            proof: gpu.proof)
    }
}
