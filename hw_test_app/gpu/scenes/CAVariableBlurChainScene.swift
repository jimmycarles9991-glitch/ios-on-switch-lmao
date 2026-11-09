import Foundation
import HWTestCore
import Metal

/// Core Animation's variable blur chain as backboardd draws a scroll pocket's: a 776x24 texture of 10 levels whose
/// level 0 holds a picture, each later level n rendered from level n - 1 of the same texture by QuartzCore's
/// variable_blur_downsample_frag (PRGhAXm_TvrdA2Xhf_Isrc into RGBA16Float, PBGRAXm_TvrdA2Xhf_Isrc into BGRA8Unorm), in
/// one command buffer. Its uniforms are bound where the function reads them (Mac reflection): the identity texture
/// matrix at vertex buffer 3, u = {source_level, dx, dy, _, clamp} at fragment buffer 1 with the source's texel step,
/// so an edge texel's taps reach past the level into the sampler's transparent border, and the chain at fragment
/// texture 3. Levels 1-9 are first cleared to a sentinel no blur of the picture makes. Every level is read back whole.
///
/// Per chain (f16, u8) one count per level, slash-separated ("0" when every level has none): <c>_nan=, <c>_inf=,
/// <c>_oor= (f16: finite but outside [0, 1]), <c>_mag= (R and B over 200/255, G under 80/255), <c>_stale= (the
/// sentinel: texels the level's pass never wrote), <c>_top= and <c>_bottom= (texels of those kinds in the level's
/// first and last row); <c>_mean= each level's mean over its channels x 255, to set the raw facade's run beside m2v's;
/// <c>_path= the chain command buffer's path. Any such texel fails the scene, naming the chain and level.
struct CAVariableBlurChainScene: GPUScene {
    let id = "gpu.blur_chain_nan"
    let name = "CA variable blur chain, 776x24 in 10 levels: NaN, magenta and stale texels per level"

    static let width = 776
    static let height = 24
    static let levels = 10

    struct Chain {
        let key: String
        let format: TexelFormat
        let state: String
        let sentinel: [Float]
    }

    static let chains = [
        Chain(key: "f16", format: .rgba16Float, state: "PRGhAXm_TvrdA2Xhf_Isrc", sentinel: [-7, 3, -7, 5]),
        Chain(key: "u8", format: .bgra8Unorm, state: "PBGRAXm_TvrdA2Xhf_Isrc", sentinel: [0, 1, 0, 0]),
    ]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let library: MTLLibrary
        do {
            library = try gpu.device.makeLibrary(URL: CAState.library)
        } catch {
            result.notes.append("no QuartzCore library here (\(GPU.describe(error)))")
            return result
        }
        for chain in Self.chains {
            guard let line = CACorpus.state(chain.state) else {
                result.expect(false, "\(chain.state) is not in CACorpus")
                continue
            }
            let pipeline: MTLRenderPipelineState
            do {
                pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
            } catch {
                result.notes.append("\(chain.key): no state (\(GPU.describe(error)))")
                continue
            }
            let texture = try gpu.texture(chain.format.metal, Self.width, Self.height, mipmaps: Self.levels)
            let picture = Patterns.opaque(Patterns.busy(width: Self.width, height: Self.height, channels: 4))
            try await gpu.upload(
                Patterns.bytes(picture, chain.format), to: texture, bytesPerRow: Self.width * chain.format.bytesPerTexel
            )
            try await gpu.run { commands in
                for level in 1..<Self.levels { Self.clear(commands, texture, level: level, to: chain.sentinel) }
            }
            let timing = try await gpu.run(tolerateError: true) { commands in
                for level in 1..<Self.levels {
                    Self.downsample(commands, pipeline, texture, level: level, colour1: chain.format.metal, gpu: gpu)
                }
            }
            result.metrics.set(chain.key + "_path", timing.path)
            var tallies: [LevelTally] = []
            for level in 0..<Self.levels {
                let (w, h) = Self.extent(level)
                let bytes = try await gpu.read(texture, bytesPerPixel: chain.format.bytesPerTexel, level: level)
                tallies.append(LevelTally(bytes, chain, width: w, height: h))
            }
            Self.report(chain, tallies, into: &result)
        }
        return result
    }

    static func extent(_ level: Int) -> (Int, Int) { (max(1, width >> level), max(1, height >> level)) }

    static func clear(_ commands: MTLCommandBuffer, _ texture: MTLTexture, level: Int, to colour: [Float]) {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = texture
        pass.colorAttachments[0].level = level
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        pass.colorAttachments[0].clearColor = MTLClearColor(
            red: Double(colour[0]), green: Double(colour[1]), blue: Double(colour[2]), alpha: Double(colour[3]))
        commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
    }

    static func downsample(
        _ commands: MTLCommandBuffer, _ pipeline: MTLRenderPipelineState, _ texture: MTLTexture, level: Int,
        colour1: MTLPixelFormat, gpu: GPU
    ) {
        let (w, h) = extent(level)
        let (sw, sh) = extent(level - 1)
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = texture
        pass.colorAttachments[0].level = level
        pass.colorAttachments[0].loadAction = .dontCare
        pass.colorAttachments[0].storeAction = .store
        pass.colorAttachments[1].texture = try? gpu.texture(
            colour1, w, h, usage: [.renderTarget], storage: .memoryless)
        pass.colorAttachments[1].loadAction = .dontCare
        pass.colorAttachments[1].storeAction = .dontCare
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(pipeline)
        CAState.bind(
            encoder, vertices: CAState.rectangle(x0: -1, y0: -1, x1: 1, y1: 1, color: SIMD4(1, 1, 1, 1)),
            source: texture)
        var texmat = SIMD4<Float>(1, 1, 0, 0)
        var u: [Float] = [Float(level - 1), 1 / Float(sw), 1 / Float(sh), 0, 0, 0, 1, 1]
        encoder.setVertexBytes(&texmat, length: MemoryLayout<SIMD4<Float>>.size, index: 3)
        encoder.setFragmentBytes(&u, length: u.count * 4, index: 1)
        for slot in 2..<4 { encoder.setFragmentTexture(texture, index: slot) }
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
    }

    static func report(_ chain: Chain, _ tallies: [LevelTally], into result: inout SceneResult) {
        var counts: [(String, (LevelTally) -> Int)] = [
            ("mag", { $0.magenta }), ("stale", { $0.stale }), ("top", { $0.top }), ("bottom", { $0.bottom }),
        ]
        if chain.format.isFloat {
            counts.insert(contentsOf: [("nan", { $0.nan }), ("inf", { $0.inf }), ("oor", { $0.oor })], at: 0)
        }
        for (name, count) in counts {
            let values = tallies.map(count)
            let text = values.allSatisfy { $0 == 0 } ? "0" : values.map { String($0) }.joined(separator: "/")
            result.metrics.set("\(chain.key)_\(name)", text)
        }
        result.metrics.set(
            chain.key + "_mean", tallies.map { String(Int(($0.mean * 255).rounded())) }.joined(separator: "/"))
        for (level, tally) in tallies.enumerated() where tally.bad > 0 {
            result.expect(false, "\(chain.key) level \(level) (\(tally.width)x\(tally.height)): \(tally.summary)")
        }
    }
}

/// One level's texels by kind; a texel counts once, under its first kind in nan, inf, stale, oor, magenta order.
struct LevelTally {
    let width: Int
    let height: Int
    var nan = 0, inf = 0, oor = 0, magenta = 0, stale = 0, top = 0, bottom = 0
    var mean: Double = 0

    init(_ bytes: [UInt8], _ chain: CAVariableBlurChainScene.Chain, width: Int, height: Int) {
        self.width = width
        self.height = height
        let size = chain.format.bytesPerTexel
        var sum = 0.0
        var finite = 0
        for y in 0..<height {
            for x in 0..<width {
                let at = (y * width + x) * size
                let v = chain.format.decode(bytes[at..<(at + size)])
                let bad: Bool
                if v.contains(where: \.isNaN) {
                    nan += 1
                    bad = true
                } else if v.contains(where: \.isInfinite) {
                    inf += 1
                    bad = true
                } else if v == chain.sentinel {
                    stale += 1
                    bad = true
                } else if v.contains(where: { $0 < -1.0 / 1024 || $0 > 1 + 1.0 / 1024 }) {
                    oor += 1
                    bad = true
                } else if v[0] > 200.0 / 255 && v[1] < 80.0 / 255 && v[2] > 200.0 / 255 {
                    magenta += 1
                    bad = true
                } else {
                    bad = false
                }
                if bad && y == 0 { top += 1 }
                if bad && y == height - 1 { bottom += 1 }
                for value in v where value.isFinite {
                    sum += Double(value)
                    finite += 1
                }
            }
        }
        mean = finite > 0 ? sum / Double(finite) : 0
    }

    var bad: Int { nan + inf + oor + magenta + stale }

    var summary: String {
        let parts = [("NaN", nan), ("Inf", inf), ("out of range", oor), ("magenta", magenta), ("stale", stale)]
        return parts.filter { $0.1 > 0 }.map { "\($0.1) \($0.0)" }.joined(separator: ", ")
            + " (first row \(top), last \(bottom))"
    }
}
