import Foundation
import HWTestCore
import HWTestGPU
import Metal

extension X1TwodProbes {
    /// SET_RENDER_SOLID_PRIM_COLOR_FORMAT's one-word formats per Metal format (cl902d.h), and the word of RGBA codes
    static func solidWord(_ format: X1Format, _ c: [UInt32]) -> (UInt32, UInt32)? {
        switch format.name {
        case "bgra8", "bgra8srgb": return (0xcf, c[3] << 24 | c[0] << 16 | c[1] << 8 | c[2])
        case "rgba8", "rgba8srgb": return (0xd5, c[3] << 24 | c[2] << 16 | c[1] << 8 | c[0])
        case "rgb10a2": return (0xd1, c[3] << 30 | c[2] << 20 | c[1] << 10 | c[0])
        case "r8": return (0xf3, c[0])
        default: return nil
        }
    }

    /// The 2D engine's UNORM name for an sRGB format's surface: the engine refuses a solid prim into A8RL8GL8BL8 or
    /// A8BL8GL8RL8 (class error 0x4a, T-v2), so an sRGB target is filled through its UNORM view with the colour encoded
    static func unormView(_ format: X1Format) -> UInt32? {
        switch format.name {
        case "bgra8srgb": return 0xcf
        case "rgba8srgb": return 0xd5
        default: return nil
        }
    }

    /// One fill: the reference state (SRCCOPY, clip at the surface) or the bare sequence of the first boots; the
    /// colour as one word in the destination's own format or as four floats (RF32_GF32_BF32_AF32); between the
    /// production 2D path's waits or, as on the first boots, with none
    struct FillStep: Equatable {
        let reference: Bool
        let word: Bool
        var bracketed = true

        var name: String { "\(reference ? "ref" : "bare")_\(word ? "word" : "f32")\(bracketed ? "" : "_open")" }
    }

    /// gpu.x1.twod_clear (clear.twod, LIMITS 2D-4): a 2D-engine solid rectangle over a target the 3D engine cleared,
    /// all but a one-texel border, held to the colour's codes (a word: its own codes; floats: each rule's rounding). An
    /// sRGB target is filled through its UNORM view (unormView) with the colour encoded, words and floats alike.
    /// The first boots' fill (bare state, four floats, no waits around it after a clear-only pass, the last GR work
    /// before the tail) left GR busy until a context-switch timeout, and on 12 Nov four floats into BGRA8 passed their
    /// waits, then GR could not save the channel's context and the kernel panicked (x1-bench3 BENCH3.md): the raw path
    /// refuses a fill in any colour format but its destination's own (raw_probe_ops.h probeSolidFill), so the probe
    /// runs the word forms only. It bisects one command buffer a step, journaled, safest first, every format by each
    /// form between the production 2D path's waits: the reference word, the bare word; then position alone: a
    /// known-good scaled copy and the reference word fill each with no waits in the fill's slot (position=). The
    /// first step a command buffer fails on ends the probe and is named (failed_at=); cells off the raw path are
    /// listed apart.
    static let clear = X1Probe(
        id: "gpu.x1.twod_clear", name: "2D-engine solid fills, bisected, against the colour's codes per format",
        probe: "clear.twod", risky: true, timeout: 150
    ) { gpu in
        let journal = ProbeJournal(probe: "gpu.x1.twod_clear")
        let bgra8 = X1Format.named(["bgra8"])[0]
        let (w, h) = (40, 24)
        var cells: [(String, X1RuleTally)] = []
        var ops = 0, borders = 0
        var failedAt: String?
        var failedStep: FillStep?
        var offPath: [String] = []
        /// The 3D clear of `target` to the border colour, then `op` at the blit encoder: its path, nil where it failed
        func run(_ label: String, _ target: MTLTexture, _ op: [String: Any]) async -> String? {
            journal.starting(label)
            do {
                let batch = try await gpu.run(forms: "") { commands in
                    let pass = X1RawProbes.pass(target, load: .clear)
                    pass.colorAttachments[0].clearColor = MTLClearColor(
                        red: 0.125, green: 0.375, blue: 0.625, alpha: 0.875)
                    commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
                    let blit = commands.makeBlitCommandEncoder()!
                    try RawProbe.engine(commands, op)
                    blit.endEncoding()
                }
                ops += batch.taken["ops"] ?? 0
                if batch.path != "raw" { offPath.append("\(label):\(batch.path)") }
                return batch.path
            } catch {
                failedAt = label
                return nil
            }
        }
        func fill(_ format: X1Format, _ step: FillStep, _ color: SIMD4<Float>) async throws -> Bool {
            let label = "\(format.name)_\(step.name)_\(color.x),\(color.y),\(color.z),\(color.w)"
            let codes = format.channels.enumerated().map { c, channel in
                X1Rounding.nearest.code(exactValue(channel, Double(color[c])), channel)
            }
            guard !step.word || solidWord(format, codes) != nil else { return true }
            let target = try gpu.texture(format.pixelFormat, w, h, usage: [.renderTarget, .shaderRead])
            var op: [String: Any] = [
                "kind": "fill", "written": target, "dx": 1, "dy": 1, "dw": w - 2, "dh": h - 2,
                "reference": step.reference ? 1 : 0, "unbracketed": step.bracketed ? 0 : 1,
            ]
            if let view = unormView(format) { op["fmt2d"] = view }
            if step.word, let (fmt, word) = solidWord(format, codes) {
                op["colorFormat"] = fmt
                op["color0"] = word
            } else {
                op["colorFormat"] = 0xc0
                for (c, channel) in format.channels.enumerated() {
                    op["color\(c)"] = Float(exactValue(channel, Double(color[c]))).bitPattern
                }
            }
            guard let path = await run(label, target, op) else {
                failedStep = step
                return false
            }
            guard path == "raw" else { return true }
            let bytes: [UInt8]
            do {
                bytes = try await gpu.read(target, bytesPerPixel: format.bytesPerTexel)
            } catch {
                failedAt = "\(label)_read"
                failedStep = step
                return false
            }
            var tally = X1RuleTally()
            let exact = format.channels.enumerated().map { c, channel in
                step.word ? channel.value(codes[c]) : exactValue(channel, Double(color[c]))
            }
            let border = format.codes(bytes, 0)
            for t in 0..<w * h {
                let (x, y) = (t % w, t / w)
                if x == 0 || y == 0 || x == w - 1 || y == h - 1 {
                    if format.codes(bytes, t) != border { borders += 1 }
                    continue
                }
                tally.add(
                    format.codes(bytes, t), exact: exact, channels: format.channels, metal: format.metalRounding, at: t)
            }
            cells.append((label, tally))
            return true
        }
        let colors: [SIMD4<Float>] = [
            SIMD4(0, 0, 0, 1), SIMD4(1, 1, 1, 1), SIMD4(0.5, 0.25, 0.75, 1),
            SIMD4(0.5 / 255, 1.5 / 255, 127.5 / 255, 0.5),
            SIMD4(0.2, 0.4, 0.6, 0.8), SIMD4(1.0 / 3, 2.0 / 3, 0.1, 0.9),
        ]
        var ran: [FillStep] = []
        outer: for (reference, word) in [(true, true), (false, true)] {
            let step = FillStep(reference: reference, word: word)
            guard try await fill(bgra8, step, colors[2]) else { break }
            ran.append(step)
            for format in X1Format.all {
                for color in colors where !(format.name == "bgra8" && color == colors[2]) {
                    guard try await fill(format, step, color) else { break outer }
                }
            }
        }
        var position = "not_run"
        if failedAt == nil {
            let source = try gpu.texture(.bgra8Unorm, w, h, usage: [.renderTarget, .shaderRead])
            let target = try gpu.texture(.bgra8Unorm, w, h, usage: [.renderTarget, .shaderRead])
            let scaled = await run(
                "bgra8_scale_open", target, ["kind": "scale", "read": source, "written": target, "unbracketed": 1])
            position = scaled == nil ? "scale_hung" : "scale_ran"
            if scaled != nil {
                let filled = try await fill(bgra8, FillStep(reference: true, word: true, bracketed: false), colors[2])
                position = filled ? "clean" : "fill_hung"
            }
        }
        var finding = finding("clear.twod", cells, ops: ops, offPath: offPath)
        finding.metrics.set("failed_at", failedAt ?? "-")
        finding.metrics.set("forms_ran", ran.map(\.name).joined(separator: ","))
        finding.metrics.set("position", position)
        finding.metrics.set("border_texels_changed", borders)
        if let failedAt, failedStep?.bracketed ?? false {
            finding.verdict = .fail
            finding.note = "a fill between the waits failed at \(failedAt)"
        } else if borders > 0 {
            finding.verdict = .fail
            finding.note = "the fill wrote outside its rectangle"
        }
        return finding
    }

    /// A colour channel's exact value as its code space holds it: sRGB channels encoded (Metal clears in linear light)
    static func exactValue(_ channel: X1Channel, _ v: Double) -> Double {
        channel == .srgb8 ? X1Channel.encoded(min(max(v, 0), 1)) : v
    }
}
