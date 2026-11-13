import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.integer_targets: every integer colour format the GM20B renders (NIL's RU*/RS* targets) cleared, then half of it
/// drawn by an integer fragment, read back word for word. A clear's double is truncated toward zero and saturated, as
/// Metal stores it (m2v clear_value.rs). Fails on any byte off or any command buffer off the raw path.
struct IntegerTargetTest: HWTest {
    let id = "gpu.raw.integer_targets"
    let name = "Integer render targets: cleared and drawn on the raw path"
    let subsystem = Subsystem.metal

    struct Case {
        let format: MTLPixelFormat
        let bits: [Int]
        let signed: Bool
    }

    static let cases: [Case] = [
        Case(format: .r8Uint, bits: [8], signed: false), Case(format: .r8Sint, bits: [8], signed: true),
        Case(format: .r16Uint, bits: [16], signed: false), Case(format: .r16Sint, bits: [16], signed: true),
        Case(format: .rg8Uint, bits: [8, 8], signed: false), Case(format: .rg8Sint, bits: [8, 8], signed: true),
        Case(format: .r32Uint, bits: [32], signed: false), Case(format: .r32Sint, bits: [32], signed: true),
        Case(format: .rg16Uint, bits: [16, 16], signed: false), Case(format: .rg16Sint, bits: [16, 16], signed: true),
        Case(format: .rgba8Uint, bits: [8, 8, 8, 8], signed: false),
        Case(format: .rgba8Sint, bits: [8, 8, 8, 8], signed: true),
        Case(format: .rgb10a2Uint, bits: [10, 10, 10, 2], signed: false),
        Case(format: .rg32Uint, bits: [32, 32], signed: false), Case(format: .rg32Sint, bits: [32, 32], signed: true),
        Case(format: .rgba16Uint, bits: [16, 16, 16, 16], signed: false),
        Case(format: .rgba16Sint, bits: [16, 16, 16, 16], signed: true),
        Case(format: .rgba32Uint, bits: [32, 32, 32, 32], signed: false),
        Case(format: .rgba32Sint, bits: [32, 32, 32, 32], signed: true),
    ]
    static let clear = MTLClearColor(red: 300.9, green: -5.5, blue: 7.99, alpha: 1e12)
    static let size = 8

    /// The word Metal's clear stores in a channel of `bits`
    static func cleared(_ v: Double, bits: Int, signed: Bool) -> UInt32 {
        let lo = signed ? -pow(2, Double(bits - 1)) : 0
        let hi = signed ? pow(2, Double(bits - 1)) - 1 : pow(2, Double(bits)) - 1
        return UInt32(truncatingIfNeeded: Int64(min(max(v.rounded(.towardZero), lo), hi)))
    }

    /// The words the draw writes: each channel's own, inside its range
    static func drawn(_ c: Case) -> [UInt32] {
        c.bits.enumerated().map { i, bits in
            let v = UInt32(truncatingIfNeeded: 0x5A + 37 * i) & UInt32(truncatingIfNeeded: (1 << min(bits, 31)) - 1)
            return c.signed && bits > 1 ? UInt32(bitPattern: -Int32(v >> 1)) : v
        }
    }

    /// One pixel's channel words, a signed format's sign-extended
    static func words(_ bytes: ArraySlice<UInt8>, _ c: Case) -> [UInt32] {
        let raw = bytes.enumerated().reduce(UInt64(0)) { $0 | UInt64($1.element) << (8 * $1.offset) }
        let bytesEach = c.bits.reduce(0, +) / 8 / c.bits.count
        var out: [UInt32] = []
        var at = 0
        for bits in c.bits {
            let field: UInt64
            if c.format == .rgb10a2Uint {
                field = raw >> UInt64(at) & ((1 << UInt64(bits)) - 1)
                at += bits
            } else {
                let start = bytes.startIndex + out.count * bytesEach
                field = bytes[start..<(start + bytesEach)].enumerated().reduce(UInt64(0)) {
                    $0 | UInt64($1.element) << (8 * $1.offset)
                }
            }
            let extended =
                c.signed && bits < 32 && field >> UInt64(bits - 1) & 1 == 1 ? field | ~((1 << UInt64(bits)) - 1) : field
            out.append(UInt32(truncatingIfNeeded: extended))
        }
        return out
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        var failed: [String] = []
        for c in Self.cases {
            let pixelBytes = c.bits.reduce(0, +) / 8
            do {
                let target = try gpu.texture(c.format, Self.size, Self.size)
                let state = try gpu.render(
                    fragment: c.signed ? "integer_sint_fragment" : "integer_uint_fragment", format: c.format)
                var value = Self.drawn(c) + Array(repeating: 0, count: 4 - c.bits.count)
                try await gpu.run { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = target
                    pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].clearColor = Self.clear
                    pass.colorAttachments[0].storeAction = .store
                    let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                    e.setRenderPipelineState(state)
                    var corners = GPU.pixelQuad(0, 0, Self.size / 2, Self.size, width: Self.size, height: Self.size)
                    e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    e.setFragmentBytes(&value, length: 16, index: 0)
                    e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    e.endEncoding()
                }
                let pixels = try await gpu.read(target, bytesPerPixel: pixelBytes)
                let clears = [Self.clear.red, Self.clear.green, Self.clear.blue, Self.clear.alpha].prefix(c.bits.count)
                let wantCleared = zip(clears, c.bits).map { Self.cleared($0, bits: $1, signed: c.signed) }
                let wantDrawn = Self.drawn(c)
                var wrong = 0
                for y in 0..<Self.size {
                    for x in 0..<Self.size {
                        let at = (y * Self.size + x) * pixelBytes
                        let got = Self.words(pixels[at..<(at + pixelBytes)], c)
                        wrong += got == (x < Self.size / 2 ? wantDrawn : wantCleared) ? 0 : 1
                    }
                }
                if wrong != 0 { failed.append("\(c.format.rawValue): \(wrong) pixels wrong") }
            } catch {
                failed.append("\(c.format.rawValue): \(GPU.describe(error))")
            }
        }
        let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "raw" }
        if !raw { failed.append("command buffers off the raw path: \(gpu.paths.text)") }
        var metrics = Metrics()
        metrics.set("formats", Self.cases.count)
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        return failed.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failed.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}
