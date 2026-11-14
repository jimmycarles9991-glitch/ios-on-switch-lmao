import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Stored one-sample depth/stencil textures in the raw path's own zeta memory: each of Metal's four formats made private
/// (render target and sampled), cleared and drawn into on the left half, and read back by the copy engine, exact; and a
/// Depth32Float_Stencil8 2D array (its slice 1) and cube (its face 4) the same way; every command buffer raw and each
/// texture in raw memory (switchRawProbeFacts raw=1). Then Depth32Float targets as the device makes them on
/// compressed zeta memory (raw_comp.h storedZetaKind): a 64x64 one drawn and read back by the copy engine and sampled by
/// the texture unit, exact, and a 1280x720 one taking 16 clear-only passes (ZBC's case), timed against one held on plain
/// memory where a debug facade can (switchRawProbePlainPrivate:). The d32f case is sampled too, as the reference read.
/// Each other format storedZetaKind compresses (compressedCases) is drawn, read back and sampled on compressed memory.
struct StoredDepthTest: HWTest {
    let id = "gpu.raw.stored_depth"
    let name = "Stored depth/stencil in raw zeta memory: exact readback, ZBC clear cost"
    let subsystem = Subsystem.metal

    static let size = 64
    static let factsSelector = NSSelectorFromString("switchRawProbeFacts:")
    static let plainSelector = NSSelectorFromString("switchRawProbePlainPrivate:")

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
            if let sampled = ran.sampledWrong {
                metrics.set(c.name + "_sampled_wrong", sampled)
                metrics.set(c.name + "_sampled_paths", ran.sampledPaths.joined(separator: "/"))
            }
            if (ran.sampledWrong ?? 0) > 0 { wrong.append(c.name + "_sampled") }
            if ran.paths.contains(where: { $0 != "raw" }) { wrong.append(c.name + "_path") }
            if !ran.facts.contains("raw=1") { wrong.append(c.name + "_facts") }
            if ran.wrongDepth + ran.wrongStencil > 0 { wrong.append(c.name) }
        }
        try await compressedArm(gpu, &metrics, &wrong)
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
        var sampledWrong: Int?
        var sampledPaths: [String] = []
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
        if c.format == .depth32Float {
            var sampling = Ran()
            ran.sampledWrong = try await sampledWrong(gpu, c, texture, &sampling)
            ran.sampledPaths = sampling.paths
        }
        ran.facts = facts(gpu, texture)
        return ran
    }

    /// The texture unit's reading of a depth texture drawAndRead drew: each texel's depth in red of an R32Float target,
    /// the texels that are not the drawn (0.25; Depth16Unorm's nearest codes) or cleared depth
    func sampledWrong(_ gpu: GPU, _ c: Case, _ texture: MTLTexture, _ ran: inout Ran) async throws -> Int {
        let size = Self.size
        let out = try gpu.texture(.r32Float, size, size)
        let pipeline = try gpu.render(fragment: "stored_depth_read_fragment", format: .r32Float)
        ran.paths.append(
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = out
                pass.colorAttachments[0].loadAction = .dontCare
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                encoder.setFragmentTexture(texture, index: 0)
                GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(0, 0, 0, 1))
                encoder.endEncoding()
            }.path)
        let red = try await gpu.read(out, bytesPerPixel: 4)
        let slack: Float = c.format == .depth16Unorm ? 2.0 / 65535 : 0
        var wrong = 0
        for y in 0..<size {
            for x in 0..<size {
                let at = (y * size + x) * 4
                let depth = Float(bitPattern: red[at..<at + 4].reversed().reduce(0) { $0 << 8 | UInt32($1) })
                if abs(depth - (x < size / 2 ? 0.25 : 1)) > slack { wrong += 1 }
            }
        }
        return wrong
    }

    /// A private depth target as the device makes it, on compressed memory where it can: compressed memory of a size is
    /// made off the render thread after its first ask, so the target is made again until it binds some (20 tries; a
    /// release facade, which answers no facts, keeps the second)
    func compressedDepth(
        _ gpu: GPU, _ format: MTLPixelFormat = .depth32Float, _ width: Int = StoredDepthTest.size,
        _ height: Int = StoredDepthTest.size
    ) async throws -> MTLTexture {
        var made = try gpu.texture(format, width, height, usage: [.renderTarget, .shaderRead])
        let known = facts(gpu, made) != "-"
        for attempt in 0..<20 where known ? !facts(gpu, made).contains("comptags=1") : attempt == 0 {
            try await Task.sleep(nanoseconds: 50_000_000)
            made = try gpu.texture(format, width, height, usage: [.renderTarget, .shaderRead])
        }
        return made
    }

    /// The formats besides Depth32Float whose stored one-sample textures bind compressed memory (raw_comp.h
    /// storedZetaKind)
    static let compressedCases = ["d16", "d32fs8"]

    /// A case on a compressed target: drawn, each aspect read back by the copy engine and its depth sampled, exact
    func compressedCase(_ gpu: GPU, _ c: Case, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        let texture = try await compressedDepth(gpu, c.format)
        var ran = Ran()
        try await drawAndRead(gpu, c, texture, &ran)
        let sampled = try await sampledWrong(gpu, c, texture, &ran)
        let made = facts(gpu, texture)
        let name = c.name + "_compressed"
        metrics.set(name + "_facts", made)
        metrics.set(name + "_wrong_depth", ran.wrongDepth)
        metrics.set(name + "_wrong_stencil", ran.wrongStencil)
        metrics.set(name + "_sampled_wrong", sampled)
        metrics.set(name + "_paths", ran.paths.joined(separator: "/"))
        if ran.wrongDepth + ran.wrongStencil > 0 { wrong.append(name) }
        if sampled > 0 { wrong.append(name + "_sampled") }
        if ran.paths.contains(where: { $0 != "raw" }) { wrong.append(name + "_path") }
        if made != "-" && !made.contains("comptags=1") { wrong.append(name + "_not_made") }
    }

    /// A 64x64 compressed Depth32Float target drawn, read back by the copy engine and sampled, exact; and GPU
    /// milliseconds of 16 clear-only passes of a 1280x720 one, and of one on plain memory where the facade can hold it there
    func compressedArm(_ gpu: GPU, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        for c in Self.cases where Self.compressedCases.contains(c.name) {
            try await compressedCase(gpu, c, &metrics, &wrong)
        }
        let small = try await compressedDepth(gpu)
        var ran = Ran()
        try await drawAndRead(gpu, Self.cases[0], small, &ran)
        let sampled = try await sampledWrong(gpu, Self.cases[0], small, &ran)
        let smallFacts = facts(gpu, small)
        metrics.set("compressed_readback_facts", smallFacts)
        metrics.set("compressed_readback_wrong", ran.wrongDepth)
        metrics.set("compressed_sampled_wrong", sampled)
        metrics.set("compressed_readback_paths", ran.paths.joined(separator: "/"))
        if ran.wrongDepth > 0 { wrong.append("compressed_readback") }
        if sampled > 0 { wrong.append("compressed_sampled") }
        if ran.paths.contains(where: { $0 != "raw" }) { wrong.append("compressed_path") }
        if smallFacts != "-" && !smallFacts.contains("comptags=1") { wrong.append("compressed_not_made") }

        let big = try await compressedDepth(gpu, .depth32Float, 1280, 720)
        metrics.set("zbc_compressed_facts", facts(gpu, big))
        var targets: [(String, MTLTexture)] = [("compressed", big)]
        if let facade = gpu.device as? NSObject, facade.responds(to: Self.plainSelector) {
            _ = facade.perform(Self.plainSelector, with: NSNumber(value: true))
            defer { _ = facade.perform(Self.plainSelector, with: NSNumber(value: false)) }
            let plain = try gpu.texture(.depth32Float, 1280, 720, usage: [.renderTarget, .shaderRead])
            targets.append(("plain", plain))
        }
        for (name, target) in targets {
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
    }
}
