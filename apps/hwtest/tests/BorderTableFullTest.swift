import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// An opaque border's header for a texture whose border texel is not the sampler's own (samplers.cpp borderVariant)
/// is made before the work that samples through it, from the device's 2048 sampler headers. An OpaqueWhite sampler of a
/// descriptor no other test makes is made first; gpu.gm20b.sampler_table's distinct samplers then take every header left;
/// a draw sampling an R8Unorm texture through the border sampler, whose border reads (1, 0, 0, 1) and so needs a header
/// of its own, fails its command buffer with MTLCommandBufferErrorOutOfMemory and the facade's call.sampler, the only
/// command buffer that fails (failed_cbs=, failed_why=).
/// Once the samplers are released the same draw completes on raw: the 4x4 texture in the middle of a 12x12 target, its
/// ring the border (shaders/OpaqueBorder.metal). Fields: table_full (no sampler could be made), full_error= the failed
/// buffer's code, full_path=, freed_path=, freed_wrong= pixels off.
struct BorderTableFullTest: HWTest {
    let id = "gpu.heap.border_table_full"
    let name = "An opaque border with no sampler header left fails out of memory, and draws once one is free"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let texel: UInt8 = 200

    static func border() -> MTLSamplerDescriptor {
        let d = MTLSamplerDescriptor()
        d.sAddressMode = .clampToBorderColor
        d.tAddressMode = .clampToBorderColor
        d.borderColor = .opaqueWhite
        d.lodMaxClamp = 3.5
        return d
    }

    func draw(
        _ gpu: GPU, _ state: MTLRenderPipelineState, _ texture: MTLTexture, _ sampler: MTLSamplerState,
        into target: MTLTexture
    ) throws -> MTLCommandBuffer {
        let buffer = try gpu.commit { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.setFragmentSamplerState(sampler, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        buffer.waitUntilCompleted()
        return buffer
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let red = try gpu.texture(.r8Unorm, 4, 4, usage: [.shaderRead])
        try await gpu.upload([UInt8](repeating: Self.texel, count: 16), to: red, bytesPerRow: 4)
        let state = try gpu.render(fragment: "opaque_border_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 12, 12)
        guard let sampler = gpu.device.makeSamplerState(descriptor: Self.border()) else {
            throw TestFailure("no border sampler")
        }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []

        var held: [MTLSamplerState] = []
        for i in 0 ..< 4096 {
            guard let s = gpu.device.makeSamplerState(descriptor: SamplerTableTest.distinct(i)) else { break }
            held.append(s)
        }
        while let s = gpu.device.makeSamplerState(descriptor: SamplerTableTest.distinct(0)) { held.append(s) }
        let full = gpu.device.makeSamplerState(descriptor: SamplerTableTest.wrapping(lodMax: 12)) == nil
        metrics.set("held_samplers", held.count)
        metrics.set("table_full", full)
        checks.append(("every sampler header taken", full))

        let failed = try draw(gpu, state, red, sampler, into: target)
        let code = (failed.error as NSError?)?.code ?? 0
        let failedPath = GM20BFacade.path(of: failed)
        metrics.set("full_error", code)
        metrics.set("full_path", failedPath)
        checks.append(
            (
                "the draw failed MTLCommandBufferErrorOutOfMemory",
                failed.status == .error && code == Int(MTLCommandBufferError.Code.outOfMemory.rawValue)
            )
        )
        checks.append(("its path is error:call.sampler", failedPath == "error:call.sampler"))

        held.removeAll()
        let freed = try draw(gpu, state, red, sampler, into: target)
        let freedPath = GM20BFacade.path(of: freed)
        metrics.set("freed_path", freedPath)
        checks.append((
            "with headers free the draw completes on raw",
            freed.status == .completed && freedPath == "gm20b"
        ))
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let wrong = (0 ..< 144).filter { at in
            let inside = (4 ..< 8).contains(at % 12) && (4 ..< 8).contains(at / 12)
            return Array(pixels[4 * at ..< 4 * at + 4]) != (inside ? [Self.texel, 0, 0, 255] : [255, 0, 0, 255])
        }.count
        metrics.set("freed_wrong", wrong)
        checks.append(("the texture and its border read Metal's texels", wrong == 0))
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(
            FailedCounts.judged(
                before: before, after: counted, expecting: ["call.sampler": 1], own: gpu.paths, &metrics
            )
        )
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
