import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Stored one-sample depth/stencil textures in the raw path's own zeta memory: each of Metal's four formats made private
/// (render target and sampled), cleared and drawn into on the left half, and read back by the copy engine, exact; and a
/// Depth32Float_Stencil8 2D array (its slice 1) and cube (its face 4) the same way; every command buffer raw and no m2v
/// twin made for any of them (switchRawProbeFacts twin=0). On a debug facade a 1280x720 Depth32Float
/// target on compressed zeta memory (switchRawProbeCompressNext) and one on plain memory each take 16 clear-only passes
/// (ZBC's case): their GPU times, and the compressed one's readback after a draw, are reported without failing the test.
struct StoredDepthTest: HWTest {
    let id = "gpu.raw.stored_depth"
    let name = "Stored depth/stencil in raw zeta memory: no m2v twin, exact readback, ZBC clear cost"
    let subsystem = Subsystem.metal

    static let size = 64
    static let factsSelector = NSSelectorFromString("switchRawProbeFacts:")
    static let compressNext = NSSelectorFromString("switchRawProbeCompressNext")

    struct Case {
        let name: String
        let format: MTLPixelFormat
        let depth: Bool, stencil: Bool
        let depthBytes: Int
        let left: [UInt8]  // the drawn half's depth texel, as the copy engine reads it back
        let right: [UInt8]  // the cleared half's
        var type: MTLTextureType = .type2D
        var slice = 0  // the layer drawn and read
    }

    static func bytes(_ f: Float) -> [UInt8] { withUnsafeBytes(of: f.bitPattern.littleEndian) { Array($0) } }
    static func bytes16(_ v: UInt16) -> [UInt8] { withUnsafeBytes(of: v.littleEndian) { Array($0) } }

    /// The drawn depth is 0.25, the clear 1; Depth16Unorm's 0.25 is 16383.75 units, so it reads 16383 or 16384
    static let cases: [Case] = [
        Case(
            name: "d32f", format: .depth32Float, depth: true, stencil: false, depthBytes: 4,
            left: bytes(0.25), right: bytes(1)),
        Case(
            name: "d16", format: .depth16Unorm, depth: true, stencil: false, depthBytes: 2,
            left: bytes16(16383), right: bytes16(65535)),
        Case(
            name: "d32fs8", format: .depth32Float_stencil8, depth: true, stencil: true, depthBytes: 4,
            left: bytes(0.25), right: bytes(1)),
        Case(name: "s8", format: .stencil8, depth: false, stencil: true, depthBytes: 0, left: [], right: []),
        Case(
            name: "d32fs8_array", format: .depth32Float_stencil8, depth: true, stencil: true, depthBytes: 4,
            left: bytes(0.25), right: bytes(1), type: .type2DArray, slice: 1),
        Case(
            name: "d32fs8_cube", format: .depth32Float_stencil8, depth: true, stencil: true, depthBytes: 4,
            left: bytes(0.25), right: bytes(1), type: .typeCube, slice: 4),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        var metrics = Metrics()
        var wrong: [String] = []
        for c in Self.cases {
            let ran = try await check(gpu, c)
            metrics.set(c.name + "_paths", ran.paths.joined(separator: "/"))
            metrics.set(c.name + "_facts", ran.facts)
            metrics.set(c.name + "_wrong_depth", ran.wrongDepth)
            metrics.set(c.name + "_wrong_stencil", ran.wrongStencil)
            if ran.paths.contains(where: { $0 != "raw" }) { wrong.append(c.name + "_path") }
            if !ran.facts.contains("raw=1") || !ran.facts.contains("twin=0") { wrong.append(c.name + "_facts") }
            if ran.wrongDepth + ran.wrongStencil > 0 { wrong.append(c.name) }
        }
        do {
            try await zbcArm(gpu, &metrics)
        } catch {
            metrics.set("zbc_arm_error", "\(error)")
        }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    func facts(_ gpu: GPU, _ texture: MTLTexture) -> String {
        guard let facade = gpu.device as? NSObject, facade.responds(to: Self.factsSelector) else { return "-" }
        return facade.perform(Self.factsSelector, with: texture)?.takeUnretainedValue() as? String ?? "-"
    }

    struct Ran {
        var paths: [String] = []
        var facts = "-"
        var wrongDepth = 0, wrongStencil = 0
    }

    /// Cleared to 1 and stencil 0, the left half drawn at 0.25 with stencil 7, then each aspect read back
    func drawAndRead(_ gpu: GPU, _ c: Case, _ texture: MTLTexture, _ ran: inout Ran) async throws {
        let size = Self.size
        let colour = try gpu.texture(.rgba8Unorm, size, size)
        let pipeline = try gpu.render(
            fragment: "solid_fragment", format: .rgba8Unorm, depth: c.depth ? c.format : .invalid,
            stencil: c.stencil ? c.format : .invalid)
        let descriptor = MTLDepthStencilDescriptor()
        descriptor.depthCompareFunction = c.depth ? .lessEqual : .always
        descriptor.isDepthWriteEnabled = c.depth
        let face = MTLStencilDescriptor()
        face.depthStencilPassOperation = .replace
        descriptor.frontFaceStencil = face
        descriptor.backFaceStencil = face
        guard let state = gpu.device.makeDepthStencilState(descriptor: descriptor) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        ran.paths.append(
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = colour
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .dontCare
                if c.depth {
                    pass.depthAttachment.texture = texture
                    pass.depthAttachment.slice = c.slice
                    pass.depthAttachment.loadAction = .clear
                    pass.depthAttachment.clearDepth = 1
                    pass.depthAttachment.storeAction = .store
                }
                if c.stencil {
                    pass.stencilAttachment.texture = texture
                    pass.stencilAttachment.slice = c.slice
                    pass.stencilAttachment.loadAction = .clear
                    pass.stencilAttachment.clearStencil = 0
                    pass.stencilAttachment.storeAction = .store
                }
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                encoder.setDepthStencilState(state)
                encoder.setStencilReferenceValue(7)
                GPU.draw(encoder, corners: GPU.quad(x0: -1, y0: -1, x1: 0, y1: 1, z: 0.25), color: SIMD4(1, 1, 1, 1))
                encoder.endEncoding()
            }.path)
        let both = c.depth && c.stencil
        if c.depth {
            let depths = try await read(
                gpu, texture, slice: c.slice, bytesPerTexel: c.depthBytes, options: both ? .depthFromDepthStencil : [],
                &ran)
            for y in 0..<size {
                for x in 0..<size {
                    let at = (y * size + x) * c.depthBytes
                    let got = Array(depths[at..<at + c.depthBytes]), want = x < size / 2 ? c.left : c.right
                    if got != want && !(c.depthBytes == 2 && x < size / 2 && got == Self.bytes16(16384)) {
                        ran.wrongDepth += 1
                    }
                }
            }
        }
        if c.stencil {
            let stencils = try await read(
                gpu, texture, slice: c.slice, bytesPerTexel: 1, options: both ? .stencilFromDepthStencil : [], &ran)
            for y in 0..<size {
                for x in 0..<size where stencils[y * size + x] != (x < size / 2 ? 7 : 0) { ran.wrongStencil += 1 }
            }
        }
    }

    func read(
        _ gpu: GPU, _ texture: MTLTexture, slice: Int = 0, bytesPerTexel: Int, options: MTLBlitOption, _ ran: inout Ran
    ) async throws -> [UInt8] {
        let size = Self.size, row = size * bytesPerTexel
        let out = try gpu.buffer(row * size)
        ran.paths.append(
            try await gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.copy(
                    from: texture, sourceSlice: slice, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                    sourceSize: MTLSize(width: size, height: size, depth: 1), to: out, destinationOffset: 0,
                    destinationBytesPerRow: row, destinationBytesPerImage: row * size, options: options)
                blit.endEncoding()
            }.path)
        return Array(UnsafeRawBufferPointer(start: out.contents(), count: row * size))
    }

    func check(_ gpu: GPU, _ c: Case) async throws -> Ran {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: c.format, width: Self.size, height: Self.size, mipmapped: false)
        descriptor.textureType = c.type
        if c.type == .type2DArray { descriptor.arrayLength = 3 }
        descriptor.storageMode = .private
        descriptor.usage = [.renderTarget, .shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor) else {
            throw TestFailure("makeTexture returned nil for \(c.name)")
        }
        var ran = Ran()
        try await drawAndRead(gpu, c, texture, &ran)
        ran.facts = facts(gpu, texture)
        return ran
    }

    /// GPU milliseconds of 16 clear-only passes of a 1280x720 Depth32Float target on compressed and on plain memory, and
    /// the compressed target's readback after a draw (the copy engine reading one-sample compressed zeta)
    func zbcArm(_ gpu: GPU, _ metrics: inout Metrics) async throws {
        guard let facade = gpu.device as? NSObject, facade.responds(to: Self.compressNext) else { return }
        _ = facade.perform(Self.compressNext)
        let compressed = try gpu.texture(.depth32Float, 1280, 720, usage: [.renderTarget, .shaderRead])
        let plain = try gpu.texture(.depth32Float, 1280, 720, usage: [.renderTarget, .shaderRead])
        metrics.set("zbc_compressed_facts", facts(gpu, compressed))
        for (name, target) in [("compressed", compressed), ("plain", plain)] {
            let clears: (MTLCommandBuffer) -> Void = { commands in
                for _ in 0..<16 {
                    let pass = MTLRenderPassDescriptor()
                    pass.renderTargetWidth = 1280
                    pass.renderTargetHeight = 720
                    pass.depthAttachment.texture = target
                    pass.depthAttachment.loadAction = .clear
                    pass.depthAttachment.clearDepth = 1
                    pass.depthAttachment.storeAction = .store
                    commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
                }
            }
            var ms = 0.0
            for _ in 0..<3 { ms += try await gpu.run(clears).gpuMilliseconds }
            metrics.set("depth_clear16_\(name)_gpu_ms", ms / 3, digits: 3)
        }
        _ = facade.perform(Self.compressNext)
        let small = try gpu.texture(.depth32Float, Self.size, Self.size, usage: [.renderTarget, .shaderRead])
        var ran = Ran()
        try await drawAndRead(gpu, Self.cases[0], small, &ran)
        metrics.set("compressed_readback_facts", facts(gpu, small))
        metrics.set("compressed_readback_wrong", ran.wrongDepth)
        metrics.set("compressed_readback_paths", ran.paths.joined(separator: "/"))
    }
}
