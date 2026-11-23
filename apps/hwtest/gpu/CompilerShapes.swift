import Foundation
import HWTestCore
import Metal

/// The fragment programs of shaders/CompilerShapes.metal drawn over an RGBA32F target, against the same code run on
/// the CPU (gpu.gm20b.sm53_layout, gpu.gm20b.float_tests).
public enum CompilerShapes {
    public static let width = 256, height = 256

    public struct Reading: Sendable {
        public var wrong = 0
        /// The first wrong pixel or value, "-" for none
        public var first = "-"
        public var micros = 0.0
        public var path = "-"
    }

    /// sm53_layout_paths of pixel (x, y)
    public static func layout(_ x: UInt32, _ y: UInt32) -> SIMD4<Float> {
        let a = x &* 7 &+ y &* 13
        var path: UInt32
        switch a & 3 {
        case 0:
            path = 1 + (x & 4 != 0 ? 10 : 20 + (y & 2 != 0 ? 100 : 0))
        case 1:
            path = 2
        default:
            path = 3 + (x > y ? 30 : x == y ? 40 : 50 + (y & 1))
        }
        var sum: UInt32 = 0
        for i in 0 ... (x & 15) {
            if (i + y) % 5 == 0 { continue }
            sum += i * (y % 9 + 1)
            if sum > 120 + (x & 63) { break }
            if (i ^ x) & 8 != 0 { sum ^= 0x55 } else { sum += 3 }
        }
        return SIMD4(Float(path), Float(sum), Float(y * 256 + x), 1)
    }

    /// The specials and every class boundary 0x00800000..inf with its neighbours, both signs
    public static let floatValues: [Float] = {
        let magnitudes: [UInt32] = [
            0, 1, 2, 0x0040_0000, 0x007f_fffe, 0x007f_ffff, 0x0080_0000, 0x0080_0001, 0x0100_0000, 0x3f7f_ffff,
            0x3f80_0000, 0x3f80_0001, 0x7f7f_fffe, 0x7f7f_ffff, 0x7f80_0000, 0x7f80_0001, 0x7fbf_ffff, 0x7fc0_0000,
            0x7fc0_0001, 0x7fff_ffff,
        ]
        return [0, 0x8000_0000].flatMap { sign in magnitudes.map { Float(bitPattern: sign | $0) } }
    }()

    /// float_classes' bits of `v` as an M4 Max writes them with Metal's default fast math (the app's metallib's): a
    /// denormal classifies as a zero of its sign; a NaN answers isnan, |x| > 0 and |x| != inf alone (x != x is false);
    /// every other value as IEEE 754 states it.
    public static func classes(_ v: Float) -> UInt32 {
        if v.isNaN { return 1 | 1 << 4 | 1 << 10 | (v.sign == .minus ? 1 << 15 : 0) }
        let v = v.isSubnormal ? Float(sign: v.sign, exponent: 0, significand: 0) : v
        let m = abs(v), small = Float.leastNormalMagnitude
        let flags: [Bool] = [
            v.isNaN, v.isInfinite, v.isFinite, v.isNormal, m > 0, m < small, m >= small, m < 1,
            m <= .greatestFiniteMagnitude,
            m == .infinity, m != .infinity, v < 0, v > 0, v == 0, v != v, v.sign == .minus,
        ]
        return flags.enumerated().reduce(0) { $0 | ($1.element ? 1 << UInt32($1.offset) : 0) }
    }

    static func target(_ gpu: GPU) throws -> MTLTexture {
        try gpu.texture(.rgba32Float, width, height, usage: [.renderTarget, .shaderRead])
    }

    static func draw(
        _ gpu: GPU, _ fragment: String, into texture: MTLTexture, _ bind: (MTLRenderCommandEncoder) -> Void
    ) async throws -> GPU.Timing {
        let state = try gpu.render(fragment: fragment, format: .rgba32Float)
        return try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            var corners = GPU.fullScreen
            encoder.setRenderPipelineState(state)
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            bind(encoder)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
    }

    static func texels(_ gpu: GPU, _ texture: MTLTexture) async throws -> [SIMD4<Float>] {
        let bytes = try await gpu.read(texture, bytesPerPixel: 16)
        return (0 ..< (width * height)).map { bytes.loaded(as: SIMD4<Float>.self, at: $0) }
    }

    public static func observeLayout(_ gpu: GPU) async throws -> Reading {
        let texture = try target(gpu)
        let timing = try await draw(gpu, "sm53_layout_paths", into: texture) { _ in }
        var reading = Reading(micros: timing.gpuMilliseconds * 1000, path: timing.facadePath)
        guard !timing.path.hasPrefix("error:") else { return Reading(wrong: -1, path: timing.facadePath) }
        let got = try await texels(gpu, texture)
        for (i, texel) in got.enumerated() {
            let want = layout(UInt32(i % width), UInt32(i / width))
            guard texel != want else { continue }
            reading.wrong += 1
            if reading.first == "-" { reading.first = "\(i % width),\(i / width):\(texel)_not_\(want)" }
        }
        return reading
    }

    /// Each value's bits as the GPU wrote them (nil where the command buffer failed)
    public static func floatBits(_ gpu: GPU) async throws -> ([UInt32]?, GPU.Timing) {
        let texture = try target(gpu)
        let values = try gpu.buffer(floatValues)
        var shape = SIMD2<UInt32>(UInt32(width), UInt32(floatValues.count))
        let timing = try await draw(gpu, "float_classes", into: texture) { encoder in
            encoder.setFragmentBuffer(values, offset: 0, index: 0)
            encoder.setFragmentBytes(&shape, length: 8, index: 1)
        }
        guard !timing.path.hasPrefix("error:") else { return (nil, timing) }
        let got = try await texels(gpu, texture)
        return (floatValues.indices.map { UInt32(got[$0].x) }, timing)
    }
}
