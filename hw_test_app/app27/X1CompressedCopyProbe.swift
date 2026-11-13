import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// copy.ce_compressed (METALMAP d2): the copy engine reading and writing a private render target over compressible
/// memory (raw_comp.h kinds, the facade's switchRawProbeCompressNext) sees and leaves plain texels through that binding,
/// as it does for the same work on a plain block-linear target. Cases, each format (BGRA8 on C32, RGBA16F on C64) and
/// size: a rendered pattern read by the copy engine; full clears to the boot ZBC table's values (comptags only) and to
/// one it lacks, read by it; a clear then a half-target draw (mixed tiles); the copy engine writing all of it and half
/// of it over rendered tiles, read by the texture unit and by itself; and copies between a compressed and a plain
/// target both ways. Every case runs on the compressed target and on a plain one; the plain one's bytes and the CPU's
/// reading are the reference.
enum X1CompressedCopyProbe {
    static let probe = X1Probe(
        id: "gpu.x1.ce_compressed", name: "The copy engine reads and writes compressed colour targets as plain texels",
        probe: "copy.ce_compressed", risky: true, timeout: 180, body: run)

    static let repeats = 24

    static func run(_ gpu: GPU) async throws -> ProbeFinding {
        let journal = ProbeJournal(probe: "gpu.x1.ce_compressed")
        var metrics = Metrics()
        metrics.set("rule", "copy.ce_compressed")
        var wrong: [String: Int] = [:]
        var firsts: [String] = []
        var compressedMade = 0
        for format in [MTLPixelFormat.bgra8Unorm, .rgba16Float] {
            let kit = try BarrierKit(gpu, format)
            for (w, h) in [(256, 256), (320, 180), (1280, 720)] {
                let size = "\(kit.name)_\(w)x\(h)"
                journal.starting(size)
                guard let compressed = try kit.target(w, h, .compressed),
                    let compressed2 = try kit.target(w, h, .compressed)
                else { continue }
                compressedMade += 1
                let plain = try kit.target(w, h, .blockLinear)!, plain2 = try kit.target(w, h, .blockLinear)!
                let cases = Cases(kit: kit, width: w, height: h)
                for repeatIndex in 0..<repeats {
                    let seed = repeatIndex * 5 + 1
                    for (name, got, want) in try await cases.all(
                        seed: seed, compressed: compressed, compressed2: compressed2, plain: plain, plain2: plain2)
                    {
                        let key = "\(name)_\(kit.name)"
                        let bad = zip(got, want).filter { $0 != $1 }.count + abs(got.count - want.count)
                        wrong[key, default: 0] += bad > 0 ? 1 : 0
                        if bad > 0 && firsts.count < 4,
                            let at = (0..<min(got.count, want.count)).first(where: { got[$0] != want[$0] })
                        {
                            let texel = at / kit.bytesPerTexel
                            firsts.append("\(key)@\(size):\(texel % w),\(texel / w):\(got[at])>\(want[at])")
                        }
                    }
                }
            }
        }
        metrics.set("compressed_targets", compressedMade)
        metrics.set("repeats", repeats)
        metrics.set("first", firsts.isEmpty ? "-" : firsts.joined(separator: ";"))
        if compressedMade == 0 {
            return ProbeFinding(.inconclusive, metrics, note: "no compressible target was made (compression off)")
        }
        return ProbeFinding.counted(wrong.mapValues { $0 }, paths: gpu.paths.paths.keys.map { $0 }, metrics: metrics)
    }

    /// The cases on one size: each answers (name, the compressed target's bytes, the reference bytes).
    struct Cases {
        let kit: BarrierKit
        let width: Int, height: Int

        var bytes: Int { width * height * kit.bytesPerTexel }

        func pattern(_ seed: Int) -> [UInt8] {
            (0..<width * height).flatMap { kit.texel($0 % width, $0 / width, seed: seed) }
        }

        func clearPass(_ commands: MTLCommandBuffer, _ t: MTLTexture, _ color: MTLClearColor) {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = t
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = color
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }

        /// The copy engine's read of `t`
        func ceRead(_ t: MTLTexture) async throws -> [UInt8] {
            try await kit.gpu.read(t, bytesPerPixel: kit.bytesPerTexel)
        }

        /// The texture unit's read of `t`: a pass copies it into a plain target, which the copy engine reads
        func shaderRead(_ t: MTLTexture) async throws -> [UInt8] {
            let into = try kit.target(width, height, .blockLinear)!
            try await kit.gpu.run { commands in
                let encoder = commands.makeRenderCommandEncoder(descriptor: BarrierKit.pass(into))!
                encoder.setRenderPipelineState(kit.tap)
                encoder.setFragmentTexture(t, index: 0)
                var params = [SIMD2<Int32>(0, 0), SIMD2(1, 1), SIMD2(0, 0), SIMD2(Int32(width), Int32(height))]
                encoder.setFragmentBytes(&params, length: 32, index: 0)
                var corners = GPU.fullScreen
                encoder.setVertexBytes(&corners, length: 64, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            return try await ceRead(into)
        }

        func ceWrite(_ bytes: [UInt8], into t: MTLTexture, rows: Int) async throws {
            let staging = try kit.gpu.buffer(bytes)
            try await kit.gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.copy(
                    from: staging, sourceOffset: 0, sourceBytesPerRow: width * kit.bytesPerTexel,
                    sourceBytesPerImage: 0,
                    sourceSize: MTLSize(width: width, height: rows, depth: 1), to: t, destinationSlice: 0,
                    destinationLevel: 0, destinationOrigin: MTLOrigin())
                blit.endEncoding()
            }
        }

        func ceCopy(_ from: MTLTexture, _ to: MTLTexture) async throws {
            try await kit.gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.copy(
                    from: from, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                    sourceSize: MTLSize(width: width, height: height, depth: 1), to: to, destinationSlice: 0,
                    destinationLevel: 0, destinationOrigin: MTLOrigin())
                blit.endEncoding()
            }
        }

        static let clears = [
            ("zbc_black", MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)),
            ("zbc_white", MTLClearColor(red: 1, green: 1, blue: 1, alpha: 1)),
            ("zbc_zero", MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)),
            ("nonzbc", MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1)),
        ]

        func all(
            seed: Int, compressed: MTLTexture, compressed2: MTLTexture, plain: MTLTexture, plain2: MTLTexture
        ) async throws -> [(String, [UInt8], [UInt8])] {
            var out: [(String, [UInt8], [UInt8])] = []
            let expected = pattern(seed)
            for t in [compressed, plain] {
                try await kit.gpu.run { kit.seed($0, t, seed: seed, spin: 0) }
            }
            out.append(("rendered_plain_vs_cpu", try await ceRead(plain), expected))
            out.append(("rendered_ce_vs_cpu", try await ceRead(compressed), expected))
            out.append(("rendered_shader_vs_cpu", try await shaderRead(compressed), expected))
            let (clearName, color) = Self.clears[seed % Self.clears.count]
            for t in [compressed, plain] { try await kit.gpu.run { clearPass($0, t, color) } }
            let clearedPlain = try await ceRead(plain)
            out.append(("\(clearName)_ce", try await ceRead(compressed), clearedPlain))
            out.append(("\(clearName)_shader", try await shaderRead(compressed), clearedPlain))
            for t in [compressed, plain] {
                try await kit.gpu.run { kit.seed($0, t, seed: seed + 1, spin: 0, window: (0, 0, width, height / 2)) }
            }
            out.append(("mixed_ce", try await ceRead(compressed), try await ceRead(plain)))
            let written = pattern(seed + 2)
            for t in [compressed, plain] { try await ceWrite(written, into: t, rows: height) }
            out.append(("ce_write_shader", try await shaderRead(compressed), written))
            out.append(("ce_write_ce", try await ceRead(compressed), written))
            for t in [compressed, plain] {
                try await kit.gpu.run { kit.seed($0, t, seed: seed + 3, spin: 0) }
                try await ceWrite(written, into: t, rows: height / 2)
            }
            out.append(("ce_half_write_shader", try await shaderRead(compressed), try await shaderRead(plain)))
            try await ceCopy(compressed, plain2)
            out.append(("ce_copy_out", try await ceRead(plain2), try await ceRead(plain)))
            try await ceCopy(plain, compressed2)
            out.append(("ce_copy_in", try await shaderRead(compressed2), try await ceRead(plain)))
            return out
        }
    }
}
