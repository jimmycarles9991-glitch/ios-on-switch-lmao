import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Depth/stencil textures past stored render targets, raw's in zeta memory (raw_zeta_cpu.h, createOwnedZeta): a
/// Depth32Float 2D array of three levels whose slice 1 a pass clears to 0.25, read back by view_read_depth_layers; a
/// read-only Depth16Unorm made; a shared Depth32Float texture replaced with 0.25 on its left half and 0.75 on its
/// right, drawn over at 0.5 with a less test that writes depth, reads back 0.25 and 0.5 through getBytes (the copy
/// engine's); a shared Stencil8 texture's bytes, a Depth32Float_Stencil8 texture's depth floats (Metal moves no stencil
/// through the CPU) and a 4x Depth32Float texture's samples (each pixel's in turn) come back as they went in. This
/// Mac's Metal does the same: m2vf-shapes2 probe/hwref/depthchain.swift, shareddepth.swift, depthcpu.swift,
/// depthms.swift. Each went to m2v before. A private heap's depth cube of levels and a shared heap's depth array are raw's
/// too, and a shared Depth32Float on compressed memory round-trips its CPU copies (comptags_shared: whether it bound some).
struct TextureShapesDepthTest: HWTest {
    let id = "gpu.raw.texture_shapes_depth"
    let name = "Shared, mipmapped, layered and read-only depth/stencil textures are raw's own"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        if let skip = ShapeArms.skip(gpu) { return skip }
        let arms = ShapeArms(gpu)
        let depthChain = arms.make(
            .depth32Float, .type2DArray, 8, 8, levels: 3, array: 2, usage: [.renderTarget, .shaderRead])
        arms.route("depth_chain", depthChain, expect: "raw")
        if let depthChain {
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.depthAttachment.texture = depthChain
                pass.depthAttachment.slice = 1
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.storeAction = .store
                pass.depthAttachment.clearDepth = 0.25
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
            let read = try await arms.drawn(depthChain, fragment: "view_read_depth_layers", width: 2, format: .r32Float)
            arms.off("depth_chain", Array(read[4..<8]), [0, 0, 0x80, 0x3e])
        }
        arms.route("depth_read_only", arms.make(.depth16Unorm, .type2D, 8, 8, usage: .shaderRead), expect: "raw")
        try await sharedDepth(arms)
        let stencil = arms.make(.stencil8, .type2D, 4, 4, storage: .shared, usage: [.renderTarget, .shaderRead])
        arms.route("shared_stencil", stencil, expect: "raw")
        if let stencil {
            arms.off("shared_stencil", try arms.cpuRoundTrip(stencil, width: 4, height: 4, bytesPerRow: 4, rows: 4), [])
        }
        let packed = arms.make(
            .depth32Float_stencil8, .type2D, 4, 4, storage: .shared, usage: [.renderTarget, .shaderRead])
        arms.route("shared_depth_stencil", packed, expect: "raw")
        if let packed {
            let floats = (0..<16).flatMap { i in withUnsafeBytes(of: Float(i) / 16) { Array($0) } }
            packed.replace(region: MTLRegionMake2D(0, 0, 4, 4), mipmapLevel: 0, withBytes: floats, bytesPerRow: 16)
            var got = [UInt8](repeating: 0, count: 64)
            packed.getBytes(&got, bytesPerRow: 16, from: MTLRegionMake2D(0, 0, 4, 4), mipmapLevel: 0)
            arms.off("shared_depth_stencil", got, floats)
        }
        let samples = arms.make(
            .depth32Float, .type2DMultisample, 4, 2, samples: 4, storage: .shared, usage: [.renderTarget, .shaderRead])
        arms.route("shared_depth_ms", samples, expect: "raw")
        if let samples {
            let floats = (0..<32).flatMap { i in withUnsafeBytes(of: Float(i) / 64) { Array($0) } }
            samples.replace(region: MTLRegionMake2D(0, 0, 4, 2), mipmapLevel: 0, withBytes: floats, bytesPerRow: 64)
            var got = [UInt8](repeating: 0, count: 128)
            samples.getBytes(&got, bytesPerRow: 64, from: MTLRegionMake2D(0, 0, 4, 2), mipmapLevel: 0)
            arms.off("shared_depth_ms", got, floats)
        }
        try heapDepth(arms)
        try await compressedShared(arms)
        return arms.outcome()
    }

    /// A private heap's Depth32Float cube of three levels and a shared heap's Depth16Unorm 2D array, its slice 1 through
    /// replaceRegion and getBytes (this Mac's Metal makes both and round-trips the slice: probe/hwref/heapdepth.swift)
    private func heapDepth(_ arms: ShapeArms) throws {
        let chain = MTLTextureDescriptor()
        chain.textureType = .typeCube
        chain.pixelFormat = .depth32Float
        chain.width = 16
        chain.height = 16
        chain.mipmapLevelCount = 3
        chain.usage = [.renderTarget, .shaderRead]
        arms.route("heap_chain", heapTexture(arms.gpu, chain), expect: "raw")
        let array = MTLTextureDescriptor()
        array.textureType = .type2DArray
        array.pixelFormat = .depth16Unorm
        array.width = 8
        array.height = 8
        array.arrayLength = 2
        array.storageMode = .shared
        array.usage = .shaderRead
        let shared = heapTexture(arms.gpu, array)
        arms.route("heap_shared", shared, expect: "raw")
        if let shared {
            arms.off(
                "heap_shared", try arms.cpuRoundTrip(shared, width: 8, height: 8, bytesPerRow: 16, rows: 8, slice: 1),
                [])
        }
    }

    private func heapTexture(_ gpu: GPU, _ d: MTLTextureDescriptor) -> MTLTexture? {
        let sized = gpu.device.heapTextureSizeAndAlign(descriptor: d)
        let heap = MTLHeapDescriptor()
        heap.size = sized.size
        heap.storageMode = d.storageMode
        return gpu.device.makeHeap(descriptor: heap)?.makeTexture(descriptor: d)
    }

    /// A shared Depth32Float made again until it binds compressed memory (StoredDepthTest's tries), whose CPU copies the
    /// copy engine writes and reads through its compressible kind: comptags_shared 1 where it bound some
    private func compressedShared(_ arms: ShapeArms) async throws {
        let probe = StoredDepthTest()
        var made = try arms.gpu.texture(.depth32Float, 64, 64, storage: .shared)
        for _ in 0..<20 where !probe.facts(arms.gpu, made).contains("comptags=1") {
            try await Task.sleep(nanoseconds: 50_000_000)
            made = try arms.gpu.texture(.depth32Float, 64, 64, storage: .shared)
        }
        arms.route("shared_compressed", made, expect: "raw")
        arms.set("comptags_shared", probe.facts(arms.gpu, made).contains("comptags=1") ? 1 : 0)
        arms.off(
            "shared_compressed", try arms.cpuRoundTrip(made, width: 64, height: 64, bytesPerRow: 256, rows: 64), [])
    }

    private func sharedDepth(_ arms: ShapeArms) async throws {
        let gpu = arms.gpu
        let depth = arms.make(.depth32Float, .type2D, 8, 8, storage: .shared, usage: [.renderTarget, .shaderRead])
        arms.route("shared_depth", depth, expect: "raw")
        guard let depth else { return }
        let halves = (0..<64).map { Float($0 % 8 < 4 ? 0.25 : 0.75) }
        depth.replace(region: MTLRegionMake2D(0, 0, 8, 8), mipmapLevel: 0, withBytes: halves, bytesPerRow: 32)
        let colour = try gpu.texture(.rgba8Unorm, 8, 8)
        let state = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm, depth: .depth32Float)
        let tests = MTLDepthStencilDescriptor()
        tests.depthCompareFunction = .less
        tests.isDepthWriteEnabled = true
        guard let depthState = gpu.device.makeDepthStencilState(descriptor: tests) else {
            throw TestFailure("no depth state")
        }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .load
            pass.depthAttachment.storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            encoder.setDepthStencilState(depthState)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            var white = SIMD4<Float>(1, 1, 1, 1)
            encoder.setFragmentBytes(&white, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        var got = [UInt8](repeating: 0, count: 256)
        depth.getBytes(&got, bytesPerRow: 32, from: MTLRegionMake2D(0, 0, 8, 8), mipmapLevel: 0)
        let want = (0..<64).flatMap { i in withUnsafeBytes(of: Float(i % 8 < 4 ? 0.25 : 0.5)) { Array($0) } }
        arms.off("shared_depth", got, want)
    }
}
