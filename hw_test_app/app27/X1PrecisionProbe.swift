import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Whether any ROP control our context sets differently from NVIDIA's golden defaults (sw_method_init) moves the
/// store or the blend: each arm sets controls with the draw and restores NVK's value after it (RawMethods), then the
/// unorm8 and RGB10A2 stores of all 65,536 halves (MAX blend over zero, as gpu.x1.rop_conversion) and three BGRA8 blend
/// pairs of gpu.x1.blend_exact are held to the chip's measured rules (X1Reference.store, chipBlend) and to Metal's.
/// The claim: no arm changes a count, so the rounding is the hardware's and no setting of ours makes it approximate.
enum X1PrecisionProbe {
    struct Arm {
        let name: String
        let set: [(UInt32, UInt32)]
        let restore: [(UInt32, UInt32)]
    }

    static let arms: [Arm] = [
        Arm(name: "base", set: [], restore: []),
        Arm(name: "csaa_off", set: [(0x15b4, 0)], restore: [(0x15b4, 1)]),
        Arm(name: "color_clamp_off", set: [(0x2600, 0)], restore: [(0x2600, 1)]),
        Arm(name: "anti_alias_off", set: [(0x1534, 0)], restore: [(0x1534, 1)]),
        Arm(name: "float_options_golden", set: [(0x19c0, 0), (0x1140, 0)], restore: [(0x19c0, 1), (0x1140, 0x10)]),
        Arm(name: "reduce_colors_on", set: [(0x0d9c, 1)], restore: [(0x0d9c, 0)]),
    ]

    static let blendPairs = ["sov", "sat", "xor"]

    static let probe = X1Probe(
        id: "gpu.x1.rop_precision",
        name: "ROP controls NVIDIA sets otherwise leave the store and blend rules unchanged",
        probe: "E3/E4 arms", risky: true, timeout: 300
    ) { gpu in
        let journal = ProbeJournal(probe: "gpu.x1.rop_precision")
        let rig = try X1BlendProbes.Rig(gpu)
        let pairs = X1BlendProbes.pairs.filter { blendPairs.contains($0.name) }
        let states = try pairs.map(rig.state)
        let stores = try [(MTLPixelFormat.bgra8Unorm, 8), (.rgb10a2Unorm, 10)].map { format, bits in
            (
                format, bits,
                try gpu.render(fragment: "x1_half_bits", format: format) { descriptor in
                    let attachment = descriptor.colorAttachments[0]!
                    attachment.isBlendingEnabled = true
                    attachment.rgbBlendOperation = .max
                    attachment.alphaBlendOperation = .max
                }
            )
        }
        var metrics = Metrics()
        var lines: [String] = []
        var base: [String] = []
        var changed: [String] = [], refused: [String] = [], silent: [String] = []
        var paths: [String] = []
        for arm in arms {
            journal.starting(
                "\(arm.name) set=\(arm.set.map { String(format: "%04x=%x", $0.0, $0.1) }.joined(separator: ","))")
            var fields: [String] = []
            var emitted = 0
            do {
                for (format, bits, state) in stores {
                    let target = try gpu.texture(format, 256, 256, usage: [.renderTarget, .shaderRead])
                    let ran = try await run(gpu, arm) { commands in
                        let encoder = commands.makeRenderCommandEncoder(
                            descriptor: X1RawProbes.pass(target, load: .clear))!
                        encoder.setRenderPipelineState(state)
                        var corners = GPU.fullScreen
                        encoder.setVertexBytes(&corners, length: 64, index: 0)
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                        encoder.endEncoding()
                    }
                    paths.append(ran.path)
                    emitted += ran.emitted
                    fields.append(
                        "store\(bits):" + storeMisses(try await gpu.read(target, bytesPerPixel: 4), bits: bits))
                }
                for (pair, state) in zip(pairs, states) {
                    let ran = try await run(gpu, arm) { rig.encode($0, state) }
                    paths.append(ran.path)
                    emitted += ran.emitted
                    let counts = try await rig.counts(pair)
                    fields.append("\(pair.name):\(counts.chip)/\(counts.separate)")
                }
            } catch {
                fields = ["refused"]
                metrics.set("\(arm.name)_why", LogLine.sanitized(String(String(describing: error).prefix(80))))
            }
            if arm.set.isEmpty {
                base = fields
            } else if fields == ["refused"] {
                refused.append(arm.name)
            } else if emitted == 0 {
                fields = ["not_emitted"]
                silent.append(arm.name)
            } else if fields != base {
                changed.append(arm.name)
            }
            lines.append("\(arm.name)=\(fields.joined(separator: "+"))")
        }
        metrics.set("rule", "fetch.blend")
        metrics.set("changed_arms", changed.isEmpty ? "-" : changed.joined(separator: ","))
        metrics.set("refused_arms", refused.isEmpty ? "-" : refused.joined(separator: ","))
        metrics.set("not_emitted_arms", silent.isEmpty ? "-" : silent.joined(separator: ","))
        metrics.set("base", base.joined(separator: "+"))
        metrics.set("arms", lines.joined(separator: ","))
        let offPath = paths.filter { $0 != "raw" }
        let judged = arms.count - 1 - refused.count - silent.count
        let verdict: ClaimVerdict =
            !offPath.isEmpty || base == ["refused"] || judged == 0 ? .inconclusive : changed.isEmpty ? .pass : .fail
        let note =
            !offPath.isEmpty
            ? "not on the raw path: \(offPath.joined(separator: ","))"
            : judged == 0
                ? "no arm's controls reached a draw" : changed.isEmpty ? "" : "a control moved the ROP: see arms"
        return ProbeFinding(verdict, metrics, note: note)
    }

    /// One command buffer with the arm's controls set around its draws; the base arm arms nothing
    static func run(
        _ gpu: GPU, _ arm: Arm, _ encode: (MTLCommandBuffer) throws -> Void
    ) async throws
        -> (path: String, emitted: Int)
    {
        if arm.set.isEmpty { return (try await gpu.run(encode).path, 0) }
        let drawn = try await RawMethods.draw(gpu, before: arm.set, after: arm.restore, encode)
        return (drawn.path, drawn.emitted)
    }

    /// "<misses of the chip's rule>/<misses of Metal's>" over the first channel of all 65,536 halves
    static func storeMisses(_ bytes: [UInt8], bits: Int) -> String {
        var chip = 0, metal = 0
        for i in 0..<65536 {
            let have = bits == 8 ? Int(bytes[i * 4 + 2]) : (Int(bytes[i * 4]) | Int(bytes[i * 4 + 1]) << 8) & 1023
            let v = Double(Float16(bitPattern: UInt16(i)))
            if have != X1Reference.store(v, bits: bits) { chip += 1 }
            if have != X1Reference.unorm(v, top: Double((1 << bits) - 1)) { metal += 1 }
        }
        return "\(chip)/\(metal)"
    }
}
