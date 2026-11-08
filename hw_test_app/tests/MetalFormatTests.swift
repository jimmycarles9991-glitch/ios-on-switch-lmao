import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Each colour format rendered from (0.2, 0.4, 0.6, 1.0) by a fragment shader (integer formats by a clear) and read
/// back raw; depth and stencil formats by clears.
struct MetalPixelFormatTest: HWTest {
    let id = "metal.formats"
    let name = "Pixel formats: render and read back"
    let subsystem = Subsystem.metal

    struct Case {
        let format: MTLPixelFormat
        let name: String
        let bytes: Int
        let clear: MTLClearColor?
        /// Decodes one pixel's bytes to channel values in the units `expected` uses.
        let decode: ([UInt8]) -> [Double]
        let expected: [Double]
        let tolerance: Double
    }

    static let value: [Float] = [0.2, 0.4, 0.6, 1.0]

    static func unorm(_ bytes: [UInt8], _ size: Int) -> [Double] {
        stride(from: 0, to: bytes.count, by: size).map { at in
            Double((0..<size).reduce(0) { $0 | Int(bytes[at + $1]) << (8 * $1) })
        }
    }

    static func floats(_ bytes: [UInt8], half: Bool) -> [Double] {
        if half {
            return stride(from: 0, to: bytes.count, by: 2).map {
                Double(Float16(bitPattern: UInt16(bytes[$0]) | UInt16(bytes[$0 + 1]) << 8))
            }
        }
        return stride(from: 0, to: bytes.count, by: 4).map { at in
            Double(Float(bitPattern: (0..<4).reduce(UInt32(0)) { $0 | UInt32(bytes[at + $1]) << (8 * $1) }))
        }
    }

    static func srgb(_ linear: Float) -> Double { Double((1.055 * pow(linear, 1 / 2.4) - 0.055) * 255) }

    static let cases: [Case] = [
        Case(
            format: .r8Unorm, name: "r8", bytes: 1, clear: nil, decode: { unorm($0, 1) }, expected: [51], tolerance: 0),
        Case(
            format: .rg8Unorm, name: "rg8", bytes: 2, clear: nil, decode: { unorm($0, 1) }, expected: [51, 102],
            tolerance: 0),
        Case(
            format: .rgba8Unorm, name: "rgba8", bytes: 4, clear: nil, decode: { unorm($0, 1) },
            expected: [51, 102, 153, 255], tolerance: 0),
        Case(
            format: .bgra8Unorm, name: "bgra8", bytes: 4, clear: nil, decode: { unorm($0, 1) },
            expected: [153, 102, 51, 255], tolerance: 0),
        Case(
            format: .rgba8Unorm_srgb, name: "rgba8_srgb", bytes: 4, clear: nil, decode: { unorm($0, 1) },
            expected: [srgb(0.2), srgb(0.4), srgb(0.6), 255], tolerance: 1),
        Case(
            format: .rgba16Unorm, name: "rgba16", bytes: 8, clear: nil, decode: { unorm($0, 2) },
            expected: [13107, 26214, 39321, 65535], tolerance: 1),
        Case(
            format: .r16Float, name: "r16f", bytes: 2, clear: nil, decode: { floats($0, half: true) }, expected: [0.2],
            tolerance: 0.0002),
        Case(
            format: .rgba16Float, name: "rgba16f", bytes: 8, clear: nil, decode: { floats($0, half: true) },
            expected: [0.2, 0.4, 0.6, 1], tolerance: 0.0005),
        Case(
            format: .r32Float, name: "r32f", bytes: 4, clear: nil, decode: { floats($0, half: false) },
            expected: [Double(Float(0.2))], tolerance: 0),
        Case(
            format: .rgba32Float, name: "rgba32f", bytes: 16, clear: nil, decode: { floats($0, half: false) },
            expected: value.map(Double.init), tolerance: 0),
        Case(
            format: .rgb10a2Unorm, name: "rgb10a2", bytes: 4, clear: nil,
            decode: { bytes in
                let word = (0..<4).reduce(UInt32(0)) { $0 | UInt32(bytes[$1]) << (8 * $1) }
                return [word & 1023, word >> 10 & 1023, word >> 20 & 1023, word >> 30].map(Double.init)
            }, expected: [204.6, 409.2, 613.8, 3], tolerance: 1),
        Case(
            format: .rg11b10Float, name: "rg11b10f", bytes: 4, clear: nil,
            decode: { bytes in
                let word = (0..<4).reduce(UInt32(0)) { $0 | UInt32(bytes[$1]) << (8 * $1) }
                func small(_ bits: UInt32, mantissa: UInt32) -> Double {
                    let exponent = Int(bits >> mantissa)
                    let fraction = Double(bits & ((1 << mantissa) - 1)) / Double(1 << mantissa)
                    return exponent == 0 ? fraction * pow(2, -14) : (1 + fraction) * pow(2, Double(exponent - 15))
                }
                return [
                    small(word & 0x7FF, mantissa: 6), small(word >> 11 & 0x7FF, mantissa: 6),
                    small(word >> 22, mantissa: 5),
                ]
            }, expected: [0.2, 0.4, 0.6], tolerance: 0.02),
        Case(
            format: .r32Uint, name: "r32u", bytes: 4,
            clear: MTLClearColor(red: 3_735_928_559, green: 0, blue: 0, alpha: 0),
            decode: { unorm($0, 4) }, expected: [3_735_928_559], tolerance: 0),
        Case(
            format: .rgba16Uint, name: "rgba16u", bytes: 8,
            clear: MTLClearColor(red: 1, green: 2, blue: 300, alpha: 65535),
            decode: { unorm($0, 2) }, expected: [1, 2, 300, 65535], tolerance: 0),
    ]

    func render(_ gpu: GPU, _ test: Case, size: Int) async throws -> [UInt8] {
        let target = try gpu.texture(test.format, size, size)
        let pipeline = test.clear == nil ? try gpu.render(fragment: "exact_fragment", format: test.format) : nil
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = test.clear ?? MTLClearColor()
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            if let pipeline {
                encoder.setRenderPipelineState(pipeline)
                GPU.draw(
                    encoder, corners: GPU.fullScreen,
                    color: SIMD4(Self.value[0], Self.value[1], Self.value[2], Self.value[3]))
            }
            encoder.endEncoding()
        }
        return try await gpu.read(target, bytesPerPixel: test.bytes)
    }

    func depthStencil(_ gpu: GPU, size: Int) async throws -> (depth: Float, stencil: UInt8) {
        let depth = try gpu.texture(.depth32Float, size, size, usage: [.renderTarget])
        let stencil = try gpu.texture(.stencil8, size, size, usage: [.renderTarget])
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 0.25
            pass.depthAttachment.storeAction = .store
            pass.stencilAttachment.texture = stencil
            pass.stencilAttachment.loadAction = .clear
            pass.stencilAttachment.clearStencil = 0x5A
            pass.stencilAttachment.storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        let depthBytes = try await gpu.read(depth, bytesPerPixel: 4)
        let stencilBytes = try await gpu.read(stencil, bytesPerPixel: 1)
        return (depthBytes.loaded(as: Float.self, at: size * size / 2), stencilBytes[size * size / 2])
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let size = 16
        var failed: [String] = []
        for test in Self.cases {
            do {
                let pixels = try await render(gpu, test, size: size)
                let got = test.decode(
                    Array(pixels[(size * size / 2 * test.bytes)..<((size * size / 2 + 1) * test.bytes)]))
                if zip(got, test.expected).contains(where: { abs($0 - $1) > test.tolerance }) {
                    failed.append("\(test.name) read \(got.map { String(format: "%g", $0) }.joined(separator: ","))")
                }
            } catch {
                failed.append("\(test.name): \(error)")
            }
        }
        let (depth, stencil) = try await depthStencil(gpu, size: size)
        if depth != 0.25 { failed.append("depth32f read \(depth)") }
        if stencil != 0x5A { failed.append("stencil8 read \(stencil)") }
        var metrics = Metrics()
        metrics.set("formats", Self.cases.count + 2)
        metrics.set("failed", failed.count)
        return failed.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failed.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}

/// The RGBA16Float render target's float-to-half conversion must round to nearest even, like Apple GPUs:
/// 0.5 is exact; 0.5 + 2^-12 is a tie between 0x3800 and 0x3801 and goes to the even 0x3800; 0.5 - 3·2^-14 is
/// nearest to 0x37ff; 0.5 - 2^-13 is a tie between 0x37ff and 0x3800 and goes to 0x3800 (truncation would give
/// 0x37ff, round-half-up 0x3801 for the second).
struct MetalHalfRoundingTest: HWTest {
    let id = "metal.rgba16f_rounding"
    let name = "RGBA16Float output rounds to nearest even"
    let subsystem = Subsystem.metal

    static let inputs: SIMD4<Float> = SIMD4(0.5, 0.5 + 0x1p-12, 0.5 - 3 * 0x1p-14, 0.5 - 0x1p-13)
    static let expected: [UInt16] = [0x3800, 0x3800, 0x37FF, 0x3800]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let target = try gpu.texture(.rgba16Float, 4, 4)
        let pipeline = try gpu.render(fragment: "exact_fragment", format: .rgba16Float)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            GPU.draw(encoder, corners: GPU.fullScreen, color: Self.inputs)
            encoder.endEncoding()
        }
        let pixels = try await gpu.read(target, bytesPerPixel: 8)
        let halves = (0..<4).map { pixels.loaded(as: UInt16.self, at: $0) }
        let text = halves.map { String(format: "%04x", $0) }.joined(separator: "_")
        var metrics = Metrics()
        metrics.set("halves", text)
        return .judged([("reads 3800_3800_37ff_3800", halves == Self.expected)], metrics, proof: gpu.proof)
    }
}

/// Block-compressed sampling: an ASTC 4x4 LDR void-extent block (one constant colour, exact in both of ASTC's 8-bit
/// and 16-bit decode modes) and, where the GPU has them, a BC1 block.
struct MetalCompressedTest: HWTest {
    let id = "metal.compressed"
    let name = "Compressed textures (ASTC, BC) sampled"
    let subsystem = Subsystem.metal

    static let astcBlock: [UInt8] = [
        0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x33, 0x33, 0x66, 0x66, 0x99, 0x99, 0xFF, 0xFF,
    ]
    /// color0 = R10 G20 B30 (5:6:5) above color1 = 0: four-colour mode, every index 0, so every texel is color0.
    static let bc1Block: [UInt8] = [0x9E, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]

    func sample(_ gpu: GPU, format: MTLPixelFormat, block: [UInt8]) async throws -> SIMD4<Float> {
        let texture = try gpu.texture(format, 4, 4, usage: [.shaderRead])
        try await gpu.upload(block, to: texture, bytesPerRow: block.count)
        let out = try gpu.buffer(16 * 16)
        let pipeline = try gpu.compute("sample_texture")
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: 4, height: 4, depth: 1), threadsPerThreadgroup: MTLSize(width: 4, height: 4, depth: 1))
            encoder.endEncoding()
        }
        return out.contents().load(fromByteOffset: 5 * 16, as: SIMD4<Float>.self)
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        let astc = try await sample(gpu, format: .astc_4x4_ldr, block: Self.astcBlock)
        metrics.set("astc", String(format: "%.3f,%.3f,%.3f,%.3f", astc.x, astc.y, astc.z, astc.w))
        let want = SIMD4<Float>(0.2, 0.4, 0.6, 1)
        checks.append(("ASTC reads 0.2,0.4,0.6,1", (0..<4).allSatisfy { abs(astc[$0] - want[$0]) < 1 / 512 }))
        if gpu.device.supportsBCTextureCompression {
            let bc1 = try await sample(gpu, format: .bc1_rgba, block: Self.bc1Block)
            metrics.set("bc1", String(format: "%.3f,%.3f,%.3f", bc1.x, bc1.y, bc1.z))
            let expected: [Float] = [10 / 31, 20 / 63, 30 / 31]
            checks.append(("BC1 reads color0", (0..<3).allSatisfy { abs(bc1[$0] - expected[$0]) < 2 / 255 }))
        }
        metrics.set("bc_supported", gpu.device.supportsBCTextureCompression)
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
