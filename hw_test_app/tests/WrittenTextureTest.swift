import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.written_textures: a fragment function writes words past 8 and 16 bits to a texture at index 1 (RenderBox's
/// uber_resolve_fragment and its packed_tex), bound in each format below, after a clear of it; the left half drawn,
/// read back word for word. The raw path compiles a written texture's stores for one format and draws a state's
/// " | wtex=" variant for any other (raw/runtime/raw_written_textures.h), so a format the probe did not guess must
/// still store Metal's words (an M4 Max's: saturated to each channel's range). Fails on any word off or any command
/// buffer off the raw path.
struct WrittenTextureTest: HWTest {
    let id = "gpu.raw.written_textures"
    let name = "Written textures: each format's words stored on the raw path"
    let subsystem = Subsystem.metal

    struct Case {
        let format: MTLPixelFormat
        let bits: [Int]
        let ushort: Bool  // written as ushort4 (the low 16 bits of each word), else as uint4
    }

    static let cases: [Case] = [
        Case(format: .r16Uint, bits: [16], ushort: true), Case(format: .rg16Uint, bits: [16, 16], ushort: true),
        Case(format: .rgba16Uint, bits: [16, 16, 16, 16], ushort: true),
        Case(format: .r32Uint, bits: [32], ushort: false), Case(format: .rgba8Uint, bits: [8, 8, 8, 8], ushort: false),
    ]
    static let size = 8
    static let clear = MTLClearColor(red: 42405, green: 1234, blue: 77, alpha: 9)
    static let specials: [UInt32] = [
        0, 1, 0xFF, 0x100, 0x3FF, 0x7E0, 0xF800, 0xFFFF, 0x1_0000, 0x1_00FF, 0x7FFF_FFFF, 0xFFFF_FFFF,
    ]

    /// The words the fragment writes at pixel `i`: every channel width's edges, and words between
    static func words(_ i: Int) -> [UInt32] {
        (0..<4).map { k in
            (i + k) % 2 == 0
                ? specials[(i / 2 + 3 * k) % specials.count]
                : UInt32(truncatingIfNeeded: (i * 4 + k) &* 0x9E37_79B9) >> UInt32(4 * k)
        }
    }

    /// The word Metal stores of `v` in an unsigned channel of `bits`: saturated to its range
    static func stored(_ v: UInt32, bits: Int) -> UInt32 {
        bits >= 32 ? v : min(v, UInt32(1 << bits) - 1)
    }

    /// The words Metal stores at pixel `i` where the fragment wrote it
    static func drawn(_ c: Case, _ i: Int) -> [UInt32] {
        zip(words(i), c.bits).map { stored(c.ushort ? $0 & 0xFFFF : $0, bits: $1) }
    }

    static func cleared(_ c: Case) -> [UInt32] {
        let clears = [clear.red, clear.green, clear.blue, clear.alpha]
        return zip(clears, c.bits).map { stored(UInt32($0), bits: $1) }
    }

    static func channels(_ bytes: ArraySlice<UInt8>, _ c: Case) -> [UInt32] {
        var at = bytes.startIndex
        return c.bits.map { bits in
            let word = bytes[at..<(at + bits / 8)].enumerated().reduce(UInt32(0)) {
                $0 | UInt32($1.element) << (8 * $1.offset)
            }
            at += bits / 8
            return word
        }
    }

    /// Pixels of `c`'s texture whose words are not Metal's
    static func wrongPixels(_ gpu: GPU, _ c: Case) async throws -> Int {
        let written = try gpu.texture(c.format, size, size, usage: [.renderTarget, .shaderRead, .shaderWrite])
        let target = try gpu.texture(.rgba8Unorm, size, size)
        let state = try gpu.render(
            fragment: c.ushort ? "written_ushort_fragment" : "written_uint_fragment", format: .rgba8Unorm)
        let pixelWords = try gpu.buffer((0..<(size * size)).map { SIMD4<UInt32>(words($0)) })
        try await gpu.run { commands in
            let clearing = MTLRenderPassDescriptor()
            clearing.colorAttachments[0].texture = written
            clearing.colorAttachments[0].loadAction = .clear
            clearing.colorAttachments[0].clearColor = clear
            clearing.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: clearing)!.endEncoding()
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(state)
            var corners = GPU.pixelQuad(0, 0, size / 2, size, width: size, height: size)
            var width = UInt32(size)
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setFragmentBuffer(pixelWords, offset: 0, index: 0)
            e.setFragmentBytes(&width, length: 4, index: 1)
            e.setFragmentTexture(written, index: 1)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
        }
        let pixelBytes = c.bits.reduce(0, +) / 8
        let pixels = try await gpu.read(written, bytesPerPixel: pixelBytes)
        var wrong = 0
        for y in 0..<size {
            for x in 0..<size {
                let i = y * size + x
                let got = channels(pixels[(i * pixelBytes)..<((i + 1) * pixelBytes)], c)
                wrong += got == (x < size / 2 ? drawn(c, i) : cleared(c)) ? 0 : 1
            }
        }
        return wrong
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        var failed: [String] = []
        for c in Self.cases {
            do {
                let wrong = try await Self.wrongPixels(gpu, c)
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
