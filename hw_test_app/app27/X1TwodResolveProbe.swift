import Foundation
import HWTestCore
import HWTestGPU
import Metal

extension X1TwodProbes {
    /// Sample codes per pixel class and sample whose averages fall at every quarter of a code: 4x .25, .5 and .75,
    /// 2x .0 and .5, per channel rotated
    static let sampleOffsets: [[UInt32]] = [[0, 0, 0, 1], [0, 1, 1, 1], [0, 0, 1, 1], [0, 1, 0, 0]]

    /// The code channel c of pixel class `set` holds at sample s
    static func sampleCode(_ channel: X1Channel, set: Int, c: Int, s: Int) -> UInt32 {
        let offset = sampleOffsets[(set + c) % 4][s]
        let seed = UInt32(set * 37 + 20 + c * 61)
        switch channel {
        case .unorm, .srgb8:
            let top = UInt32((1 << (channel.codeBits ?? 8)) - 1)
            return 1 + (seed * (top - 2) / 256) % (top - 1) + offset
        case .float(let m): return 14 << UInt32(m) | (seed % ((1 << UInt32(m)) - 2)) + offset
        case .float32: return Float(0.5 + Double(seed) / 1024).bitPattern + offset
        }
    }

    /// gpu.x1.twod_resolve (resolve.twod, METALMAP d5): a multisampled pass's resolve by the 2D engine, its sample
    /// grid read as one image at 2:1 (corner origin at half a step), held to the average of the codes the samples
    /// store, which one-sample draws of each sample's colour measure first; resolve.draw (the pack's resolve state,
    /// today's route) is judged the same way where the format has one. T-v2 found no single rounding for the 4x cells:
    /// each is also read as two rounded 2:1 steps (PairReading, either pairing, each rule), and the rules on the line come
    /// from whichever reading fits it with fewer misses (`pair_readings=` lists the cells a pair reading explained). Under resolve=twod the facade needs no resolve
    /// state for the format; a cell whose command buffer still leaves the raw path is listed apart.
    static let resolve = X1Probe(
        id: "gpu.x1.twod_resolve", name: "2D-engine MSAA resolves against the samples' average, per format and count",
        probe: "resolve.twod", risky: true, timeout: 150
    ) { gpu in
        let journal = ProbeJournal(probe: "gpu.x1.twod_resolve")
        var cells: [(String, X1RuleTally)] = [], draws: [(String, X1RuleTally)] = []
        var explained: [(String, X1RuleTally)] = []
        var linear: [String] = [], offPath: [String] = [], readings: [String] = []
        var ops = 0
        let (w, h) = (96, 64)
        for format in X1Format.named(["bgra8", "rgba8", "bgra8srgb", "rgba8srgb", "rgba16f", "rgb10a2", "rg11b10f"]) {
            let (stored, calibration) = try await storedSamples(gpu, format)
            if let off = calibration.first(where: { $0 != "raw" }) {
                offPath.append("\(format.name)_samples:\(off)")
                continue
            }
            for samples in [2, 4] where gpu.device.supportsTextureSampleCount(samples) {
                let name = "\(format.name)_\(samples)x"
                journal.starting(name)
                let colors = try sampleColors(gpu, format)
                let pipeline = try gpu.render(
                    fragment: "x1_sample_fragment", format: format.pixelFormat, samples: samples)
                for arm in ["twod", "draw"] where arm == "twod" || format.name != "rgb10a2" {
                    let ms = try gpu.texture(
                        format.pixelFormat, w, h, usage: [.renderTarget], samples: samples,
                        storage: .memoryless)
                    let into = try gpu.texture(format.pixelFormat, w, h, usage: [.renderTarget, .shaderRead])
                    let batch = try await gpu.run(forms: "resolve=\(arm)") { commands in
                        let pass = MTLRenderPassDescriptor()
                        pass.colorAttachments[0].texture = ms
                        pass.colorAttachments[0].resolveTexture = into
                        pass.colorAttachments[0].loadAction = .clear
                        pass.colorAttachments[0].storeAction = .multisampleResolve
                        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                        encoder.setRenderPipelineState(pipeline)
                        encoder.setFragmentBuffer(colors, offset: 0, index: 0)
                        var corners = GPU.fullScreen
                        encoder.setVertexBytes(&corners, length: 64, index: 0)
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                        encoder.endEncoding()
                    }
                    if arm == "twod" { ops += batch.taken["resolves"] ?? 0 }
                    guard batch.path == "raw" else {
                        offPath.append("\(name)_\(arm):\(batch.path)")
                        continue
                    }
                    let bytes = try await gpu.read(into, bytesPerPixel: format.bytesPerTexel)
                    var tally = X1RuleTally(), codesTally = X1RuleTally()
                    let pairReadings = samples == 4 && arm == "twod" ? separable : []
                    var pairTallies = [X1RuleTally](repeating: X1RuleTally(), count: pairReadings.count)
                    for t in 0..<w * h {
                        let set = (t % w + 3 * (t / w)) % 4
                        let got = format.codes(bytes, t)
                        tally.add(
                            got, exact: average(format, stored[set], samples, srgbLinear: true),
                            channels: format.channels, metal: format.metalRounding, at: t)
                        codesTally.add(
                            got, exact: average(format, stored[set], samples, srgbLinear: false),
                            channels: format.channels, metal: format.metalRounding, at: t)
                        for (k, reading) in pairReadings.enumerated() {
                            pairTallies[k].add(
                                got, exact: pairAverage(format, stored[set], reading),
                                channels: format.channels, metal: format.metalRounding, at: t)
                        }
                    }
                    if arm == "twod" {
                        cells.append((name, tally))
                        let best = pairTallies.indices.min {
                            pairTallies[$0].fewestMisses < pairTallies[$1].fewestMisses
                        }
                        if let best, pairTallies[best].fewestMisses < tally.fewestMisses {
                            explained.append((name, pairTallies[best]))
                            readings.append("\(name):\(pairReadings[best].name):\(pairTallies[best].text)")
                        } else {
                            explained.append((name, tally))
                        }
                        if format.channels.contains(.srgb8) { linear.append("\(name):\(codesTally.text)") }
                    } else {
                        draws.append((name, tally))
                    }
                }
            }
        }
        var finding = finding("resolve.twod", cells, ops: ops, offPath: offPath, explained: explained)
        finding.metrics.set("pair_readings", readings.isEmpty ? "-" : readings.joined(separator: ","))
        finding.metrics.set("srgb_codes_cells", linear.isEmpty ? "-" : linear.joined(separator: ","))
        finding.metrics.set("draw_rules", ruleSummary(draws))
        finding.metrics.set("draw_cells", draws.map { "\($0.0):\($0.1.text)" }.joined(separator: ","))
        return finding
    }

    /// Per pixel class, per sample, the codes the format stores for that sample's colour: one-sample draws of each
    /// colour (exact_fragment) into pixel `set` of a 4 x 1 target; and the paths of their command buffers
    static func storedSamples(_ gpu: GPU, _ format: X1Format) async throws -> ([[[UInt32]]], [String]) {
        let pipeline = try gpu.render(fragment: "exact_fragment", format: format.pixelFormat)
        var out = [[[UInt32]]](repeating: [[UInt32]](repeating: [], count: 4), count: 4)
        var paths: [String] = []
        for s in 0..<4 {
            let target = try gpu.texture(format.pixelFormat, 4, 1, usage: [.renderTarget, .shaderRead])
            let ran = try await gpu.run { commands in
                let encoder = commands.makeRenderCommandEncoder(descriptor: X1RawProbes.pass(target, load: .clear))!
                encoder.setRenderPipelineState(pipeline)
                for set in 0..<4 {
                    var color = sampleColor(format, set: set, s: s)
                    var corners = GPU.pixelQuad(set, 0, set + 1, 1, width: 4, height: 1)
                    encoder.setVertexBytes(&corners, length: 64, index: 0)
                    encoder.setFragmentBytes(&color, length: 16, index: 0)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            paths.append(ran.path)
            let bytes = try await gpu.read(target, bytesPerPixel: format.bytesPerTexel)
            for set in 0..<4 { out[set][s] = format.codes(bytes, set) }
        }
        return (out, paths)
    }

    /// Sample s's colour in pixel class `set`, as the shader writes it (sRGB channels in linear light)
    static func sampleColor(_ format: X1Format, set: Int, s: Int) -> SIMD4<Float> {
        var v = SIMD4<Float>(0, 0, 0, 1)
        for (c, channel) in format.channels.enumerated() {
            let code = sampleCode(channel, set: set, c: c, s: s)
            v[c] = Float(channel == .srgb8 ? X1Channel.linear(channel.value(code)) : channel.value(code))
        }
        return v
    }

    /// x1_sample_fragment's colours: class set's sample s at set * 4 + s
    static func sampleColors(_ gpu: GPU, _ format: X1Format) throws -> MTLBuffer {
        try gpu.buffer((0..<16).map { sampleColor(format, set: $0 / 4, s: $0 % 4) })
    }

    /// A 4x resolve read as two 2:1 steps: samples averaged in pairs, each pair rounded to a code by `rounding`, the two
    /// codes averaged
    struct PairReading {
        let pairs: [(Int, Int)]
        let rounding: X1Rounding

        var name: String { "p\(pairs[0].1)_\(rounding.rawValue)" }
    }

    static let separable: [PairReading] = [[(0, 1), (2, 3)], [(0, 2), (1, 3)]].flatMap { pairs in
        X1Rounding.allCases.map { PairReading(pairs: pairs, rounding: $0) }
    }

    /// The exact value PairReading leaves for the last rounding; sRGB colour channels in their codes' space
    static func pairAverage(_ format: X1Format, _ stored: [[UInt32]], _ r: PairReading) -> [Double] {
        format.channels.enumerated().map { c, channel in
            let halves = r.pairs.map { a, b in
                channel.value(r.rounding.code((channel.value(stored[a][c]) + channel.value(stored[b][c])) / 2, channel))
            }
            return (halves[0] + halves[1]) / 2
        }
    }

    /// The exact average of the first `samples` samples' values per channel; sRGB colour channels averaged as linear
    /// light and encoded again (Metal's resolve) or as codes
    static func average(_ format: X1Format, _ stored: [[UInt32]], _ samples: Int, srgbLinear: Bool) -> [Double] {
        format.channels.enumerated().map { c, channel in
            let values = (0..<samples).map { channel.value(stored[$0][c]) }
            guard channel == .srgb8 && srgbLinear else { return values.reduce(0, +) / Double(samples) }
            return X1Channel.encoded(values.map(X1Channel.linear).reduce(0, +) / Double(samples))
        }
    }
}
