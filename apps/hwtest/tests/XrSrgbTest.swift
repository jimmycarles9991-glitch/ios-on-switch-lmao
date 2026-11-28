import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// The extended-range sRGB formats on the driver, which holds each in an RGBA16Float stand-in (BGR10_XR_sRGB 555 for
/// gpu.gm20b.xr_srgb, BGRA10_XR_sRGB 553 for gpu.gm20b.xr_srgb_alpha), against Metal (XrSrgbScene.swift, goldens measured
/// on an M4 Max). decode: every code of every channel sampled reads Metal's value within its tolerance (decode_off
/// texel channels off, decode_worst the largest difference). load_store: a pass that loads and stores with no draw
/// keeps every word, alpha codes past [0, 1] included, as Metal does (load_store_off bytes off). The texture is made
/// with shaderWrite: an M4 Max's blits into a compressible 552/553 texture of 16 texels or more a side clamp alpha to
/// 384..894 (gpu.gm20b.bgra10xrsrgb_empty_pass's measurement), a pass with no draw never does. drawn: an unblended draw over a clear stores Metal's codes (drawn_off texels; 553's texel
/// 0, 0.5 in every channel, reads c0bdc0bdc0bdc09f) and samples back as Metal reads them (drawn_sampled_off).
/// blended: source-over over Metal's drawn words, within one code of Metal's (blended_off texels past it,
/// blended_worst). cleared: four clears store Metal's codes (cleared_off; cleared= the words where one is off). 555's
/// drawn_pad= and cleared_pad= the values of the two bits above its codes (Metal writes 3), reported, not judged.
/// compressed_upload (553 alone, reported): a blit from a buffer into a 64x64 ShaderRead|RenderTarget texture of alpha
/// codes i % 1024, read back by a blit; an M4 Max keeps such a texture compressed and clamps its alpha to 384..894 in
/// the copy (compressed_upload_kept 2044, compressed_upload_clamped 2052), and the driver's copies clamp it too.
/// Every command buffer raw, and none failed (failed_cbs=).
struct XrSrgbTest: HWTest {
    let id: String
    let name: String
    let format: XrSrgbFormat
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let bgr = XrSrgbTest(
        id: "gpu.gm20b.xr_srgb", name: "BGR10_XR_sRGB reads, stores, draws, blends and clears as Metal does, on raw",
        format: .bgr
    )
    static let bgra = XrSrgbTest(
        id: "gpu.gm20b.xr_srgb_alpha",
        name: "BGRA10_XR_sRGB reads, stores, draws, blends and clears as Metal does, alpha linear, on raw",
        format: .bgra
    )

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var sections: [(String, (GPU, inout Metrics, inout [(String, Bool)]) async throws -> Void)] = [
            ("decode", decodeAndKeep), ("drawn", drawn), ("blended", blended), ("cleared", cleared),
        ]
        if format.alpha { sections.append(("compressed_upload", compressedUpload)) }
        for (section, body) in sections {
            do {
                try await body(gpu, &metrics, &checks)
            } catch {
                checks.append(("\(section): \(GPU.describe(error))", false))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: ["oracle", "paths", "failed_cbs"])
        return .judged(checks, metrics, proof: gpu.proof)
    }

    var golden: (drawn: [UInt64], blended: [UInt64], cleared: [UInt64]) {
        format.alpha
            ? (XrSrgb.drawn553, XrSrgb.blended553, XrSrgb.cleared553)
            : (XrSrgb.drawn555.map(UInt64.init), XrSrgb.blended555.map(UInt64.init), XrSrgb.cleared555.map(UInt64.init))
    }

    func bytes(_ words: [UInt64]) -> [UInt8] {
        words.flatMap { word in (0 ..< format.bytes).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) } }
    }

    func words(_ bytes: [UInt8]) -> [UInt64] {
        stride(from: 0, to: bytes.count, by: format.bytes).map { at in
            (0 ..< format.bytes).reduce(UInt64(0)) { $0 | UInt64(bytes[at + $1]) << (8 * $1) }
        }
    }

    /// Sampled texels against Metal's values of `codes`: the channels off and the largest difference
    func sampled(_ texels: [SIMD4<Float>], _ codes: [[Int]]) -> (off: Int, worst: Float) {
        var off = 0
        var worst: Float = 0
        for (texel, code) in zip(texels, codes) {
            for channel in 0 ..< format.channels {
                let want = XrSrgb.decoded(code[channel], alpha: channel == 3)
                let difference = abs(texel[channel] - want)
                worst = max(worst, difference)
                if !(difference <= XrSrgb.tolerance(want)) { off += 1 }
            }
        }
        return (off, worst)
    }

    func decodeAndKeep(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        let every = XrSrgb.everyCode(format)
        let texture = try gpu.texture(format.format, 32, 32, usage: [.renderTarget, .shaderRead, .shaderWrite])
        try await gpu.upload(every, to: texture, bytesPerRow: 32 * format.bytes)
        let decode = sampled(try await gpu.texels(texture), format.codes(every))
        metrics.set("decode_off", decode.off)
        metrics.set("decode_worst", Double(decode.worst), digits: 6)
        checks.append(("every code samples as Metal reads it", decode.off == 0))
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        let kept = every
        let off = zip(try await gpu.read(texture, bytesPerPixel: format.bytes), kept).filter { $0 != $1 }.count
        metrics.set("load_store_off", off)
        checks.append(("a load and store with no draw keeps Metal's words", off == 0))
    }

    func draw(
        _ gpu: GPU, _ values: [SIMD4<Float>], into target: MTLTexture, blend: Bool
    ) async throws {
        let state = try gpu.render(fragment: "texel_values", format: format.format) {
            guard blend, let a = $0.colorAttachments[0] else { return }
            a.isBlendingEnabled = true
            a.sourceRGBBlendFactor = .sourceAlpha
            a.destinationRGBBlendFactor = .oneMinusSourceAlpha
            a.sourceAlphaBlendFactor = .one
            a.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        }
        let data = try gpu.buffer(values)
        var width = UInt32(XrSrgb.side)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = blend ? .load : .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBuffer(data, offset: 0, index: 0)
            encoder.setFragmentBytes(&width, length: 4, index: 1)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
    }

    func drawn(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        let n = XrSrgb.side
        let target = try gpu.texture(format.format, n, n)
        try await draw(gpu, XrSrgb.drawn, into: target, blend: false)
        let got = try await gpu.read(target, bytesPerPixel: format.bytes)
        let off = zip(format.codes(got), format.codes(bytes(golden.drawn))).filter { $0 != $1 }.count
        metrics.set("drawn_off", off)
        if !format.alpha { metrics.set("drawn_pad", pads(words(got))) }
        checks.append(("an unblended draw stores Metal's codes", off == 0))
        if format.alpha {
            let oracle = got[0 ..< 8].map { String(format: "%02x", $0) }.joined()
            metrics.set("oracle", oracle)
            checks.append(("0.5 in every channel stores c0bdc0bdc0bdc09f", oracle == "c0bdc0bdc0bdc09f"))
        }
        let read = sampled(try await gpu.texels(target), format.codes(bytes(golden.drawn)))
        metrics.set("drawn_sampled_off", read.off)
        checks.append(("the drawn texture samples as Metal reads its words", read.off == 0))
    }

    func blended(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        let n = XrSrgb.side
        let target = try gpu.texture(format.format, n, n)
        try await gpu.upload(bytes(golden.drawn), to: target, bytesPerRow: n * format.bytes)
        try await draw(gpu, XrSrgb.blended, into: target, blend: true)
        let got = format.codes(try await gpu.read(target, bytesPerPixel: format.bytes))
        let want = format.codes(bytes(golden.blended))
        let worst = zip(got, want).map { zip($0, $1).map { abs($0 - $1) }.max() ?? 0 }
        metrics.set("blended_off", worst.filter { $0 > 1 }.count)
        metrics.set("blended_worst", worst.max() ?? 0)
        checks.append(("a source-over draw within one code of Metal's", worst.allSatisfy { $0 <= 1 }))
    }

    func cleared(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        var got: [UInt64] = []
        for colour in XrSrgb.clears {
            let target = try gpu.texture(format.format, XrSrgb.side, XrSrgb.side)
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = colour
                pass.colorAttachments[0].storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
            let texels = Set(words(try await gpu.read(target, bytesPerPixel: format.bytes)))
            got.append(texels.count == 1 ? texels.first! : 0)
        }
        let off = zip(format.codes(bytes(got)), format.codes(bytes(golden.cleared))).filter { $0 != $1 }.count
        metrics.set("cleared_off", off)
        if off > 0 { metrics.set("cleared", got.map { String($0, radix: 16) }.joined(separator: ",")) }
        if !format.alpha { metrics.set("cleared_pad", pads(got)) }
        checks.append(("each clear stores Metal's codes in every texel", off == 0))
    }

    func compressedUpload(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        let side = 64
        let texture = try gpu.texture(format.format, side, side, usage: [.shaderRead, .renderTarget])
        let bytes = (0 ..< (side * side)).flatMap { format.texel([500, 500, 500, $0 % 1024]) }
        try await gpu.upload(bytes, to: texture, bytesPerRow: side * format.bytes)
        let codes = format.codes(try await gpu.read(texture, bytesPerPixel: format.bytes))
        var kept = 0, clamped = 0
        for (i, c) in codes.enumerated() {
            let alpha = i % 1024
            if c[3] == alpha { kept += 1 } else if c[3] == min(max(alpha, 384), 894) { clamped += 1 }
        }
        metrics.set("compressed_upload_kept", kept)
        metrics.set("compressed_upload_clamped", clamped)
    }

    /// BGR10_XR_sRGB's two bits above the codes, each value seen (Metal writes 3)
    func pads(_ words: [UInt64]) -> String {
        Set(words.map { $0 >> 30 & 3 }).sorted().map(String.init).joined(separator: "/")
    }
}
