import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Render passes into R8Unorm_sRGB, A1BGR5Unorm, ABGR4Unorm and RGB9E5Float targets, formats the GM20B renders none of
/// (raw_format_standin.h: a stand-in each pass; before, pass.fmt sent every such pass to m2v, whose NVK renders none of
/// them either). Each format's passes: a clear; one draw; a loaded texture drawn over its left part; three blended draws over
/// a clear. Against an M4 Max's rules (probe/fmt): A1BGR5 and ABGR4 store a drawn channel's nearest half's nearest code and a
/// clear's nearest code, ties up; RGB9E5 truncates a draw's mantissas and rounds a clear's; R8Unorm_sRGB holds 8-bit sRGB
/// codes after every draw; generateMipmapsForTexture: of each (the "mips" pass) as an M4 Max steps a level (probe/fmt
/// miporacle.mm: RGB9E5 exact, A1BGR5 and ABGR4 within one code at odd steps, R8Unorm_sRGB within one code at 1.8% of
/// texels). off_<format>_<pass> counts texels past the pass's bound (0 to pass): none for clears, draws and
/// loads, one code a channel for blends (for RGB9E5, which Metal truncates between draws, a mantissa unit a draw and one
/// for the clear), one code for every R8Unorm_sRGB pass (the GM20B's own sRGB encoder); paths every command buffer's path
/// (raw/N to pass).
struct FormatTargetTest: HWTest {
    let id = "gpu.raw.format_targets"
    let name = "Passes into R8Unorm_sRGB, A1BGR5Unorm, ABGR4Unorm and RGB9E5Float targets on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let formats: [UInt] = [11, 41, 42, 93]
    static let passes = ["clear", "draw", "load", "blend", "mips"]
    static let (width, height) = (64, 32)

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for format in Self.formats {
            for pass in Self.passes {
                let key = "\(format)_\(pass)"
                do {
                    let off = try await exercise(gpu, format, pass)
                    metrics.set("off_" + key, off)
                    checks.append(("\(key): every texel within its bound of Metal's", off == 0))
                } catch {
                    metrics.set("off_" + key, "failed")
                    checks.append(("\(key): \(GPU.describe(error))", false))
                }
            }
        }
        let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "raw" }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        checks.append(("every command buffer raw", raw))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// The texels of one pass of `kind` into a target of `format` past the pass's bound of Metal's rule.
    func exercise(_ gpu: GPU, _ format: UInt, _ kind: String) async throws -> Int {
        guard let pixelFormat = MTLPixelFormat(rawValue: format) else { throw TestFailure("no format \(format)") }
        if kind == "mips" { return try await mips(gpu, format, pixelFormat) }
        let (w, h) = (Self.width, Self.height)
        let bytes = format == 11 ? 1 : format == 93 ? 4 : 2
        let texture = try gpu.texture(pixelFormat, w, h, usage: [.renderTarget, .shaderRead], storage: .shared)
        let blended = kind == "blend"
        let state = try gpu.render(fragment: "texel_values", format: pixelFormat) { d in
            let a = d.colorAttachments[0]!
            a.isBlendingEnabled = blended
            a.sourceRGBBlendFactor = .sourceAlpha
            a.destinationRGBBlendFactor = .oneMinusSourceAlpha
            a.sourceAlphaBlendFactor = .one
            a.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        }
        let draws = kind == "clear" ? [] : (0..<(blended ? 3 : 1)).map { Self.values(format, w * h, UInt64($0 + 1)) }
        let buffers = try draws.map { try gpu.buffer($0) }
        let clear = MTLClearColor(red: 0.31, green: 0.52, blue: format == 93 ? 7.3 : 0.73, alpha: 0.6)
        let loaded = Self.loadedWords(format, w * h)
        if kind == "load" {
            loaded.withUnsafeBytes {
                texture.replace(
                    region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: $0.baseAddress!,
                    bytesPerRow: w * bytes)
            }
        }
        let drawnWidth = kind == "load" ? 24 : w
        var width = UInt32(w)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = kind == "load" ? .load : .clear
            pass.colorAttachments[0].clearColor = clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            if !buffers.isEmpty {
                encoder.setRenderPipelineState(state)
                var corners = GPU.quad(x0: -1, y0: -1, x1: Float(2 * drawnWidth) / Float(w) - 1, y1: 1, z: 0.5)
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentBytes(&width, length: 4, index: 1)
                for data in buffers {
                    encoder.setFragmentBuffer(data, offset: 0, index: 0)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
            }
            encoder.endEncoding()
        }
        var got = [UInt8](repeating: 0, count: w * h * bytes)
        got.withUnsafeMutableBytes {
            texture.getBytes($0.baseAddress!, bytesPerRow: w * bytes, from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)
        }
        let clearHeld = SIMD4<Float>(Float(clear.red), Float(clear.green), Float(clear.blue), Float(clear.alpha))
        var off = 0
        for i in 0..<(w * h) {
            var stored: UInt32 = 0
            for k in 0..<bytes { stored |= UInt32(got[i * bytes + k]) << (8 * k) }
            let drawn = i % w < drawnWidth && !draws.isEmpty
            let want: UInt32
            let bound: Int
            if !drawn && kind == "load" {
                want = Self.loadedWord(format, loaded, i)
                bound = format == 11 ? 1 : 0
            } else if format == 11 {
                want = Self.srgbCode(drawn ? draws.map { $0[i] } : [], clear: clearHeld)
                bound = 1
            } else {
                want = Self.word(format, draws.map { $0[i] }, clear: clearHeld, drawn: drawn)
                bound = blended ? (format == 93 ? draws.count + 1 : 1) : 0
            }
            off += Self.apart(format, stored, want) > bound ? 1 : 0
        }
        return off
    }

    /// The texels of each level generateMipmapsForTexture: made past one code of Metal's step from the level above (RGB9E5
    /// past none)
    func mips(_ gpu: GPU, _ format: UInt, _ pixelFormat: MTLPixelFormat) async throws -> Int {
        let (w, h) = (Self.width, Self.height)
        let bytes = format == 11 ? 1 : format == 93 ? 4 : 2
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: pixelFormat, width: w, height: h, mipmapped: true)
        descriptor.usage = [.shaderRead, .renderTarget]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor) else { throw TestFailure("no texture") }
        let top = Self.loadedWords(format, w * h)
        top.withUnsafeBytes {
            texture.replace(
                region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: w * bytes)
        }
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.generateMipmaps(for: texture)
            blit.endEncoding()
        }
        func words(_ level: Int, _ lw: Int, _ lh: Int) -> [UInt32] {
            var raw = [UInt8](repeating: 0, count: lw * lh * bytes)
            raw.withUnsafeMutableBytes {
                texture.getBytes(
                    $0.baseAddress!, bytesPerRow: lw * bytes, from: MTLRegionMake2D(0, 0, lw, lh), mipmapLevel: level)
            }
            return (0..<(lw * lh)).map { i in (0..<bytes).reduce(0) { $0 | UInt32(raw[i * bytes + $1]) << (8 * $1) } }
        }
        var off = 0
        var (aw, ah) = (w, h)
        var above = words(0, w, h)
        for level in 1..<texture.mipmapLevelCount {
            let (lw, lh) = (Swift.max(1, aw / 2), Swift.max(1, ah / 2))
            let got = words(level, lw, lh)
            for y in 0..<lh {
                for x in 0..<lw {
                    let want = Self.mipWord(format, above, aw, ah, lw, lh, x, y)
                    off += Self.apart(format, got[y * lw + x], want) > (format == 93 ? 0 : 1) ? 1 : 0
                }
            }
            (aw, ah, above) = (lw, lh, got)
        }
        return off
    }

    /// Metal's step of texel (x, y) of a level from the level above (pack/mip.metal's taps, raw_format_standin.h's rules)
    static func mipWord(
        _ format: UInt, _ above: [UInt32], _ aw: Int, _ ah: Int, _ lw: Int, _ lh: Int, _ x: Int, _ y: Int
    ) -> UInt32 {
        func axis(_ p: Int, _ s: Int, _ d: Int) -> (Int, Int, UInt32) {
            let at = (2 * p + 1) * s - d
            return (
                Swift.min(at / (2 * d), s - 1), Swift.min(at / (2 * d) + 1, s - 1),
                UInt32((at % (2 * d) * 256 + d) / (2 * d))
            )
        }
        let (x0, x1, wx) = axis(x, aw, lw)
        let (y0, y1, wy) = axis(y, ah, lh)
        let taps = [above[y0 * aw + x0], above[y0 * aw + x1], above[y1 * aw + x0], above[y1 * aw + x1]]
        func code(_ shift: UInt32, _ mask: UInt32, _ ties: UInt32) -> UInt32 {
            let c = taps.map { $0 >> shift & mask }
            let n = (c[0] * (256 - wx) + c[1] * wx) * (256 - wy) + (c[2] * (256 - wx) + c[3] * wx) * wy
            let (base, rest) = (n >> 16, n & 0xffff)
            return base + (rest > 0x8000 || (rest == 0x8000 && (ties >> base & 1) == 1) ? 1 : 0)
        }
        let (fx, fy) = (Float(wx) / 256, Float(wy) / 256)
        func blend(_ v: [Float]) -> Float {
            let top = v[1] * fx + v[0] * (1 - fx), bottom = v[3] * fx + v[2] * (1 - fx)
            return bottom * fy + top * (1 - fy)
        }
        switch format {
        case 42: return [0, 4, 8, 12].reduce(0) { $0 | code($1, 15, 0xff) << $1 }
        case 41: return [1, 6, 11].reduce(code(0, 1, 0)) { $0 | code($1, 31, 0x5555_ff00) << $1 }
        case 93:
            let decoded = taps.map { rgb9e5Floats($0) }
            let v = SIMD3<Float>((0..<3).map { k in Float(Float16(blend(decoded.map { $0[k] }))) })
            return rgb9e5(v)
        default:
            let linear = taps.map { c -> Float in
                let s = Float(c) / 255
                return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4)
            }
            let v = Swift.min(Swift.max(blend(linear), 0), 1)
            let s = v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055
            return UInt32((s * 255).rounded(.toNearestOrEven))
        }
    }

    /// Each draw's colour of every pixel: unit floats (RGB9E5's up to 4, a few edge values), alpha in [0, 1]
    static func values(_ format: UInt, _ count: Int, _ seed: UInt64) -> [SIMD4<Float>] {
        var state = seed
        func next() -> Float {
            state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
            return Float(state >> 40) / Float(1 << 24)
        }
        return (0..<count).map { _ in
            let scale: Float = format == 93 ? 4 : 1
            return SIMD4(next() * scale, next() * scale, next() * scale, next())
        }
    }

    /// The words a loaded texture holds: every code of A1BGR5 and ABGR4, RGB9E5 of its encoder's own words
    static func loadedWords(_ format: UInt, _ count: Int) -> [UInt8] {
        (0..<count).flatMap { i -> [UInt8] in
            let v = UInt32(truncatingIfNeeded: i &* 2_654_435_761)
            switch format {
            case 11: return [UInt8(v >> 24)]
            case 93:
                let word = rgb9e5(SIMD3(Float(v & 1023) / 97, Float(v >> 10 & 1023) / 211, Float(v >> 20 & 4095) / 911))
                return [UInt8(word & 255), UInt8(word >> 8 & 255), UInt8(word >> 16 & 255), UInt8(word >> 24)]
            default: return [UInt8(v >> 16 & 255), UInt8(v >> 24)]
            }
        }
    }

    static func loadedWord(_ format: UInt, _ loaded: [UInt8], _ i: Int) -> UInt32 {
        let bytes = format == 11 ? 1 : format == 93 ? 4 : 2
        var word: UInt32 = 0
        for k in 0..<bytes { word |= UInt32(loaded[i * bytes + k]) << (8 * k) }
        return word
    }

    /// A drawn channel's code: its nearest half, clamped (a NaN 0), then the nearest code
    static func drawnCode(_ v: Float, _ max: Float) -> UInt32 {
        let h = Float(Float16(v))
        return h > 0 ? UInt32((Swift.min(h, 1) * max).rounded(.toNearestOrEven)) : 0
    }

    /// A clear's code of a texture Metal compresses: the nearest code, a tie up
    static func clearCode(_ v: Float, _ max: Float) -> UInt32 {
        v > 0 ? UInt32((Swift.min(v, 1) * max + 0.5).rounded(.down)) : 0
    }

    /// RGB9E5: the largest channel's exponent shared, mantissas truncated or rounded
    static func rgb9e5(_ input: SIMD3<Float>, rounded: Bool = false) -> UInt32 {
        let c = SIMD3<Float>((0..<3).map { input[$0] > 0 ? Swift.min(input[$0], 65408) : 0 })
        let m = Swift.max(c.x, Swift.max(c.y, c.z))
        var e = m > 0 ? Swift.max(0, Int(m.bitPattern >> 23 & 255) - 127 + 16) : 0
        var scale = Float(sign: .plus, exponent: e - 24, significand: 1)
        if rounded && (m / scale + 0.5).rounded(.down) >= 512 {
            e += 1
            scale *= 2
        }
        var word = UInt32(e) << 27
        for k in 0..<3 {
            let mantissa = rounded ? (c[k] / scale + 0.5).rounded(.down) : (c[k] / scale).rounded(.down)
            word |= UInt32(mantissa) << (9 * k)
        }
        return word
    }

    static func rgb9e5Floats(_ w: UInt32) -> SIMD3<Float> {
        let scale = Float(sign: .plus, exponent: Int(w >> 27) - 24, significand: 1)
        return SIMD3(Float(w & 511), Float(w >> 9 & 511), Float(w >> 18 & 511)) * scale
    }

    /// The word Metal stores of a texel: the clear's (a texture Metal compresses), or the draws' blended over it in floats
    static func word(_ format: UInt, _ draws: [SIMD4<Float>], clear: SIMD4<Float>, drawn: Bool) -> UInt32 {
        if !drawn {
            switch format {
            case 42:
                return clearCode(clear.x, 15) << 12 | clearCode(clear.y, 15) << 8 | clearCode(clear.z, 15) << 4
                    | clearCode(clear.w, 15)
            case 41:
                return clearCode(clear.x, 31) << 11 | clearCode(clear.y, 31) << 6 | clearCode(clear.z, 31) << 1
                    | clearCode(clear.w, 1)
            default: return rgb9e5(SIMD3(clear.x, clear.y, clear.z), rounded: true)
            }
        }
        var v = draws.count > 1 ? held(format, clear) : draws[0]
        if draws.count > 1 {
            for s in draws {
                let rgb = SIMD3(s.x, s.y, s.z) * s.w + SIMD3(v.x, v.y, v.z) * (1 - s.w)
                v = SIMD4(rgb, s.w + v.w * (1 - s.w))
            }
        }
        switch format {
        case 42:
            return drawnCode(v.x, 15) << 12 | drawnCode(v.y, 15) << 8 | drawnCode(v.z, 15) << 4 | drawnCode(v.w, 15)
        case 41:
            return drawnCode(v.x, 31) << 11 | drawnCode(v.y, 31) << 6 | drawnCode(v.z, 31) << 1 | drawnCode(v.w, 1)
        default: return rgb9e5(SIMD3(v.x, v.y, v.z))
        }
    }

    /// A cleared texel as the pass holds it before its blends
    static func held(_ format: UInt, _ clear: SIMD4<Float>) -> SIMD4<Float> {
        switch format {
        case 42: return SIMD4((0..<4).map { Float(clearCode(clear[$0], 15)) / 15 })
        case 41:
            return SIMD4(
                Float(clearCode(clear.x, 31)) / 31, Float(clearCode(clear.y, 31)) / 31,
                Float(clearCode(clear.z, 31)) / 31, Float(clearCode(clear.w, 1)))
        default: return SIMD4(rgb9e5Floats(rgb9e5(SIMD3(clear.x, clear.y, clear.z), rounded: true)), 1)
        }
    }

    /// R8Unorm_sRGB's code: 8-bit sRGB after the clear and after every draw, a draw over another blended in linear
    static func srgbCode(_ draws: [SIMD4<Float>], clear: SIMD4<Float>) -> UInt32 {
        func encode(_ v: Float) -> UInt32 {
            let c = Swift.min(Swift.max(v, 0), 1)
            let s = c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055
            return UInt32((s * 255).rounded(.toNearestOrEven))
        }
        func decode(_ code: UInt32) -> Float {
            let s = Float(code) / 255
            return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4)
        }
        if draws.count == 1 { return encode(draws[0].x) }
        return draws.reduce(encode(clear.x)) { code, s in encode(s.x * s.w + decode(code) * (1 - s.w)) }
    }

    /// How far two words lie apart: the largest channel's codes, or RGB9E5's values over the smaller exponent's unit
    static func apart(_ format: UInt, _ a: UInt32, _ b: UInt32) -> Int {
        switch format {
        case 11: return abs(Int(a) - Int(b))
        case 93:
            let unit = Float(sign: .plus, exponent: Int(Swift.min(a >> 27, b >> 27)) - 24, significand: 1)
            let (x, y) = (rgb9e5Floats(a), rgb9e5Floats(b))
            return (0..<3).map { Int((abs(x[$0] - y[$0]) / unit).rounded(.up)) }.max()!
        default:
            let (shifts, masks): ([UInt32], [UInt32]) =
                format == 42 ? ([12, 8, 4, 0], [15, 15, 15, 15]) : ([11, 6, 1, 0], [31, 31, 31, 1])
            return (0..<4).map { abs(Int(a >> shifts[$0] & masks[$0]) - Int(b >> shifts[$0] & masks[$0])) }.max()!
        }
    }
}
