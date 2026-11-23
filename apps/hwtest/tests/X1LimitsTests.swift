import Foundation
import HWTestCore
import HWTestGPU
import Metal
import os

/// kits/SwitchGM20B/caps/limits.tsv viewport_dimension (32768, the clip WIDTH and scissor XMAX fields' range): a 64x64
/// BGRA8 target drawn by a triangle over all of clip space through viewport 0 of origin (-16352, -16352) and size
/// 32768, under scissor (0, 0, 64, 64). Each texel is limits_viewport_fragment's colour for its viewport-space place,
/// computed here: R = 128 + floor(x + 0.5 - 32), G likewise in y (viewport_32768_off 0; viewport_32768: texel (32, 32),
/// the viewport's centre, reads (128, 128)). Then origin (-32704, -32704) and size 65535, past the 16-bit field: its
/// texel (32, 32) and drawn texels reported (viewport_65535, a finding). An M4 Max draws none of either (nor any
/// viewport of 12288 or more: c38-audit hwtests12/mac/x1_limits/OUTPUT.txt), so the expectation is the GPU's own claim.
struct X1ViewportsTest: HWTest {
    let id = "gpu.x1.viewports"
    let name = "A 32768-texel viewport maps its centre to the centre texel it covers"
    let subsystem = Subsystem.metal

    static let side = 64

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let state = try gpu.render(
            vertex: "limits_viewport_vertex", fragment: "limits_viewport_fragment", format: .bgra8Unorm
        )
        let wide = try await Self.drawn(gpu, state, origin: -16352, size: 32768)
        let widest = try await Self.drawn(gpu, state, origin: -32704, size: 65535)
        var off = 0
        for y in 0 ..< Self.side {
            for x in 0 ..< Self.side where Array(wide.pixel(x, y, width: Self.side)) != Self.placed(x, y) { off += 1 }
        }
        let centre = Array(wide.pixel(32, 32, width: Self.side))
        let widestCentre = Array(widest.pixel(32, 32, width: Self.side))
        var metrics = Metrics()
        metrics.set("viewport_32768", centre == [0, 128, 128, 255])
        metrics.set("viewport_32768_off", off)
        metrics.set("viewport_65535", "\(widestCentre[2]),\(widestCentre[1])")
        let widestDrawn = stride(from: 0, to: widest.count, by: 4).filter { widest[$0 + 2] != 0 }.count
        metrics.set("viewport_65535_drawn", widestDrawn)
        return Outcome.judged(
            [
                ("texel (32, 32) of the 32768 viewport reads its centre (128, 128)", centre == [0, 128, 128, 255]),
                ("every texel of the 32768 viewport at its place (\(off) off)", off == 0),
            ], metrics, proof: gpu.proof
        ).onRawPath(gpu)
    }

    /// The BGRA8 bytes limits_viewport_fragment gives texel (x, y) of the 32768 viewport centred on (32, 32)
    static func placed(_ x: Int, _ y: Int) -> [UInt8] { [0, UInt8(96 + y), UInt8(96 + x), 255] }

    static func drawn(
        _ gpu: GPU, _ state: MTLRenderPipelineState, origin: Double, size: Double
    ) async throws -> [UInt8] {
        let target = try gpu.texture(.bgra8Unorm, side, side)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            encoder.setViewport(
                MTLViewport(originX: origin, originY: origin, width: size, height: size, znear: 0, zfar: 1)
            )
            encoder.setScissorRect(MTLScissorRect(x: 0, y: 0, width: side, height: side))
            var half = SIMD2<Float>(Float(size / 2), Float(size / 2))
            encoder.setVertexBytes(&half, length: 8, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            encoder.endEncoding()
        }
        return try await gpu.read(target, bytesPerPixel: 4)
    }
}

/// limits.tsv threads_per_threadgroup (1024): limits_threads_kernel's pipeline answers maxTotalThreadsPerThreadgroup
/// 1024 (maxtotal_answer), and one threadgroup of 32x32 and one of 1024x1 each write all 1024 words, word i holding i
/// (all_1024_written_32x32, all_1024_written_1024x1; an M4 Max: 0 of 1024 off, c38-audit
/// hwtests12/mac/x1_limits/OUTPUT.txt).
struct X1ThreadsPerThreadgroupTest: HWTest {
    let id = "gpu.x1.threads_per_threadgroup"
    let name = "A threadgroup of 1024 threads runs every thread"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kernel = try gpu.compute("limits_threads_kernel")
        var metrics = Metrics()
        metrics.set("maxtotal_answer", kernel.maxTotalThreadsPerThreadgroup)
        var checks = [("maxTotalThreadsPerThreadgroup 1024", kernel.maxTotalThreadsPerThreadgroup == 1024)]
        let shapes = [
            ("32x32", MTLSize(width: 32, height: 32, depth: 1)), ("1024x1", MTLSize(width: 1024, height: 1, depth: 1)),
        ]
        for (name, shape) in shapes {
            let out = try gpu.buffer(4096)
            memset(out.contents(), 0xff, 4096)
            try await gpu.run { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(kernel)
                encoder.setBuffer(out, offset: 0, index: 0)
                encoder.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1), threadsPerThreadgroup: shape)
                encoder.endEncoding()
            }
            let words = out.contents().bindMemory(to: UInt32.self, capacity: 1024)
            let wrong = (0 ..< 1024).filter { words[$0] != UInt32($0) }.count
            metrics.set("all_1024_written_\(name)", wrong == 0)
            metrics.set("words_off_\(name)", wrong)
            checks.append(("\(name): every word written with its index (\(wrong) off)", wrong == 0))
        }
        return Outcome.judged(checks, metrics, proof: gpu.proof).onRawPath(gpu)
    }
}

/// limits.tsv texture_cube_dimension (16384) and texture_array_layers (2048): an R8Unorm cube of 16384 and a 2D array
/// of 2048 layers of 4x4, private, each face or layer cleared by a render pass of its own to a code of its own (face f
/// 40(f + 1), layer l l >> 3, so a layer index that lost a high bit reads another code), read by a kernel at the last
/// face's (16383, 16383) and the last layer's (3, 3): cube_last_face and array_last_layer 1 (an M4 Max: 240 and 255).
/// The cube holds 1.6 GB: where os_proc_available_memory leaves under it and cubeMargin, it is not made
/// (cube_skipped_mb, the memory there was), reported and not judged; a nil texture is reported as the device's answer.
struct X1TextureExtentsTest: HWTest {
    let id = "gpu.x1.texture_extents"
    let name = "A 16384 cube and a 2048-layer array hold their last face and layer"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let cubeSide = 16384, layers = 2048
    static let cubeMargin = 512 << 20

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        let array = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .r8Unorm, width: 4, height: 4, mipmapped: false
        )
        array.textureType = .type2DArray
        array.arrayLength = Self.layers
        array.usage = [.renderTarget, .shaderRead]
        array.storageMode = .private
        let lastLayer = SIMD3<UInt32>(3, 3, UInt32(Self.layers - 1))
        let layerCode = try await Self.cleared(gpu, array, slices: Self.layers, at: lastLayer) { $0 >> 3 }
        metrics.set("array_code", layerCode ?? -1)
        metrics.set("array_last_layer", layerCode == (Self.layers - 1) >> 3)
        checks.append(("the 2048-layer array made and its last layer read back (\(layerCode ?? -1))", layerCode == 255))
        let cubeBytes = Self.cubeSide * Self.cubeSide * 6
        let available = Int(os_proc_available_memory())
        if available < cubeBytes + Self.cubeMargin {
            metrics.set("cube_skipped_mb", available >> 20)
        } else {
            let cube = MTLTextureDescriptor.textureCubeDescriptor(
                pixelFormat: .r8Unorm, size: Self.cubeSide, mipmapped: false
            )
            cube.usage = [.renderTarget, .shaderRead]
            cube.storageMode = .private
            let at = SIMD3<UInt32>(UInt32(Self.cubeSide - 1), UInt32(Self.cubeSide - 1), 5)
            let faceCode = try await Self.cleared(gpu, cube, slices: 6, at: at) { 40 * ($0 + 1) }
            metrics.set("cube_code", faceCode ?? -1)
            metrics.set("cube_last_face", faceCode == 240)
            checks.append(("the 16384 cube made and its last face read back (\(faceCode ?? -1))", faceCode == 240))
        }
        return Outcome.judged(checks, metrics, proof: gpu.proof).onRawPath(gpu)
    }

    /// The code read at `at` after each slice was cleared to `code(slice)`; nil where the texture was not made
    static func cleared(
        _ gpu: GPU, _ descriptor: MTLTextureDescriptor, slices: Int, at: SIMD3<UInt32>, code: (Int) -> Int
    ) async throws -> Int? {
        guard let texture = gpu.device.makeTexture(descriptor: descriptor) else { return nil }
        let reader = try gpu.compute(descriptor.textureType == .typeCube ? "limits_cube_kernel" : "limits_array_kernel")
        let out = try gpu.buffer(16)
        for first in stride(from: 0, to: slices, by: 256) {
            try await gpu.run { commands in
                for slice in first ..< min(first + 256, slices) {
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = texture
                    pass.colorAttachments[0].slice = slice
                    pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].clearColor = MTLClearColor(
                        red: Double(code(slice)) / 255, green: 0, blue: 0, alpha: 1
                    )
                    pass.colorAttachments[0].storeAction = .store
                    commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
                }
            }
        }
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(reader)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            var place = at
            encoder.setBytes(&place, length: MemoryLayout<SIMD3<UInt32>>.stride, index: 1)
            let one = MTLSize(width: 1, height: 1, depth: 1)
            encoder.dispatchThreads(one, threadsPerThreadgroup: one)
            encoder.endEncoding()
        }
        return Int((out.contents().load(as: Float.self) * 255).rounded())
    }
}

/// kits/SwitchGM20B/caps/features.tsv MME-2 (graded host: icb_test runs switch_gm20b_macros.c build_icb_draw in
/// Mesa's MME simulator): an indirect command buffer of eight draws, command k covering column k of a 64x8 target in
/// code 30(k + 1), executed with executeCommandsInBuffer:indirectBuffer: whose range a compute pass of the same command
/// buffer writes just before as location 2, length 3 (the buffer held 0, 8 at its commit). drawn_commands "2,3,4":
/// those commands alone drew, each in its code (an M4 Max: the same OUTPUT.txt).
struct X1Mme2Test: HWTest {
    let id = "gpu.x1.mme_2"
    let name = "An indirect command buffer runs the range a compute pass wrote just before"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let descriptor = MTLIndirectCommandBufferDescriptor()
        descriptor.commandTypes = [.draw]
        descriptor.inheritPipelineState = true
        descriptor.inheritBuffers = true
        guard let icb = gpu.device.makeIndirectCommandBuffer(descriptor: descriptor, maxCommandCount: 8, options: [])
        else { throw TestFailure("no indirect command buffer of draws") }
        for k in 0 ..< 8 {
            icb.indirectRenderCommandAt(k).drawPrimitives(
                .triangle, vertexStart: 6 * k, vertexCount: 6, instanceCount: 1, baseInstance: 0
            )
        }
        let state = try gpu.render(
            vertex: "limits_column_vertex", fragment: "limits_column_fragment", format: .bgra8Unorm
        ) { $0.supportIndirectCommandBuffers = true }
        let ranger = try gpu.compute("limits_range_kernel")
        let range = try gpu.buffer([UInt32(0), 8])
        let target = try gpu.texture(.bgra8Unorm, 64, 8)
        try await gpu.run { commands in
            let compute = commands.makeComputeCommandEncoder()!
            compute.setComputePipelineState(ranger)
            compute.setBuffer(range, offset: 0, index: 0)
            var written = SIMD2<UInt32>(2, 3)
            compute.setBytes(&written, length: 8, index: 1)
            let one = MTLSize(width: 1, height: 1, depth: 1)
            compute.dispatchThreads(one, threadsPerThreadgroup: one)
            compute.endEncoding()
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            encoder.executeCommandsInBuffer(icb, indirectBuffer: range, offset: 0)
            encoder.endEncoding()
        }
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let reds = (0 ..< 8).map { Int(Array(pixels.pixel(8 * $0 + 4, 4, width: 64))[2]) }
        let drawn = (0 ..< 8).filter { reds[$0] != 0 }
        let inCode = drawn.allSatisfy { reds[$0] == 30 * ($0 + 1) }
        var metrics = Metrics()
        metrics.set("drawn_commands", drawn.isEmpty ? "-" : drawn.map(String.init).joined(separator: ","))
        metrics.set("column_codes", reds.map(String.init).joined(separator: ","))
        return Outcome.judged(
            [("commands 2, 3 and 4 alone drew, each in its code", drawn == [2, 3, 4] && inCode)], metrics,
            proof: gpu.proof
        ).onRawPath(gpu)
    }
}
