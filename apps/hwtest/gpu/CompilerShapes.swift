import Foundation
import HWTestCore
import HWTestGPU
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

    /// The halves whose pairs powr_specials takes: every special and class boundary, and curve exponents
    public static let powrHalves: [UInt16] = [
        0x0000, 0x8000, 0x0001, 0x8001, 0x03FF, 0x0400, 0x3BFF, 0x3C00, 0x3C01, 0xBC00, 0x3800, 0x4000, 0x4066,
        0x36AB, 0x7BFF, 0xFBFF, 0x7C00, 0xFC00, 0x7E00, 0xFE00,
    ]

    public static let powrPairs: [SIMD2<UInt16>] = powrHalves.flatMap { x in powrHalves.map { SIMD2(x, $0) } }

    /// Metal's `powr` of two halves at a special operand, as an M3 and an M4 answer it (metal2vulkan's
    /// math::fast_powr_special, measured there): a NaN for a NaN operand, x below zero (-0 is not), 0 to the 0 or
    /// inf of a zero exponent and 1 to an infinite one; otherwise, where log2(x) or y is infinite, 0 or inf by the
    /// signs of log2(x) and y. nil where the answer is finite.
    public static func powrSpecial(_ x: Float16, _ y: Float16) -> Float? {
        if x.isNaN || y.isNaN || x < 0 || (y == 0 && (x == 0 || x == .infinity)) || (y.isInfinite && x == 1) {
            return .nan
        }
        guard (x == 0 || x.isInfinite) || y.isInfinite else { return nil }
        let logNegative = x == 0 || (x.isFinite && x < 1)
        return logNegative != (y.sign == .minus) ? 0 : .infinity
    }

    /// Whether `got` is Metal's powr of the pair: its special answer exactly (a NaN any NaN), else within two half ulps
    /// of the value correctly rounded to half
    public static func powrRight(_ x: Float16, _ y: Float16, _ got: Float) -> Bool {
        if let want = powrSpecial(x, y) {
            return want.isNaN ? got.isNaN : got == want && got.sign == want.sign
        }
        let want = Float16(Float(pow(Double(x), Double(y)))), half = Float16(got)
        guard Float(half) == got, !want.isNaN, half.sign == want.sign else { return false }
        return abs(Int(half.bitPattern) - Int(want.bitPattern)) <= 2
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

extension CompilerShapes {
    /// fmax of two halves as Apple's GPUs answer it (an M1, host gpu_test SignedZeroTests): x where x > y or y is a
    /// NaN, y otherwise, so a NaN loses and a tie of +0 and -0 gives y
    public static func metalMax(_ x: Float16, _ y: Float16) -> Float16 { y.isNaN || x > y ? x : y }

    /// fmin likewise: x where x < y or y is a NaN, y otherwise
    public static func metalMin(_ x: Float16, _ y: Float16) -> Float16 { y.isNaN || x < y ? x : y }

    /// Whether `got` is `want` as the GPU wrote it to an RGBA32F target: the same half, zeros by sign, a NaN any NaN
    public static func sameHalf(_ got: Float, _ want: Float16) -> Bool {
        want.isNaN ? got.isNaN : Float(want) == got && got.sign == want.sign
    }

    /// half_min_max's tests of a pair: abs(x) < y, plus two for x < 0 (a NaN compares false)
    public static func curveTests(_ x: Float16, _ y: Float16) -> Float {
        Float(abs(x) < y ? 1 : 0) + Float(x < 0 ? 2 : 0)
    }

    /// half_min_max's fmax, fmin, clamp and tests of each pair (nil where the command buffer failed)
    public static func minMaxAnswers(_ gpu: GPU) async throws -> ([SIMD4<Float>]?, GPU.Timing) {
        let texture = try target(gpu)
        let pairs = try gpu.buffer(powrPairs)
        var shape = SIMD2<UInt32>(UInt32(width), UInt32(powrPairs.count))
        let timing = try await draw(gpu, "half_min_max", into: texture) { encoder in
            encoder.setFragmentBuffer(pairs, offset: 0, index: 0)
            encoder.setFragmentBytes(&shape, length: 8, index: 1)
        }
        guard !timing.path.hasPrefix("error:") else { return (nil, timing) }
        let got = try await texels(gpu, texture)
        return (powrPairs.indices.map { got[$0] }, timing)
    }

    /// powr_specials' answer to each pair (nil where the command buffer failed)
    public static func powrAnswers(_ gpu: GPU) async throws -> ([Float]?, GPU.Timing) {
        let texture = try target(gpu)
        let pairs = try gpu.buffer(powrPairs)
        var shape = SIMD2<UInt32>(UInt32(width), UInt32(powrPairs.count))
        let timing = try await draw(gpu, "powr_specials", into: texture) { encoder in
            encoder.setFragmentBuffer(pairs, offset: 0, index: 0)
            encoder.setFragmentBytes(&shape, length: 8, index: 1)
        }
        guard !timing.path.hasPrefix("error:") else { return (nil, timing) }
        let got = try await texels(gpu, texture)
        return (powrPairs.indices.map { got[$0].x }, timing)
    }
}
