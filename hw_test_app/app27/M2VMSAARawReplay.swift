import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.m2v.msaa_fetch's raw arms. Nothing on the m2v path writes SET_ANTI_ALIAS_ENABLE (0x1534) or 0x15b4 (envytools'
/// CSAA_ENABLE) per draw: NVK sets 0x1534 to 1 once at context init and never writes 0x15b4, which keeps the golden
/// context's 1 (sw_method_init). So these arms draw gpu.x1.twod_resolve's per-sample pass (x1_sample_fragment at 4x into
/// a memoryless target: one pass per sample, no interlock), which raw draws clean, with SET_PIXEL_SHADER_INTERLOCK_CONTROL
/// and the two controls written around the draw (RawMethods), one factor at a time, and put back after it. The shader
/// takes no interlock ticket: a kill under pixel detection is the state pair's alone, and an ok does not clear an
/// interlocked shader drawn so.
enum M2VMSAARawReplay {
    static let interlockControl: UInt32 = 0x1224
    static let antiAliasEnable: UInt32 = 0x1534
    static let csaaEnable: UInt32 = 0x15b4
    static let held: [UInt32: UInt32] = [interlockControl: 0, antiAliasEnable: 1, csaaEnable: 1]
    static let conflictPixel: UInt32 = 2
    static let conflictSample: UInt32 = 1

    static let arms: [(String, [(UInt32, UInt32)])] = [
        ("r_smp", [(interlockControl, conflictSample)]),
        ("r_aa0", [(antiAliasEnable, 0)]),
        ("r_csaa0", [(csaaEnable, 0)]),
        ("r_px", [(interlockControl, conflictPixel)]),
        ("r_px_aa0", [(interlockControl, conflictPixel), (antiAliasEnable, 0)]),
        ("r_px_csaa0", [(interlockControl, conflictPixel), (csaaEnable, 0)]),
    ]

    /// x1_sample_fragment's colours: every sample of a pixel its own
    static let colors: [SIMD4<Float>] = (0..<16).map { i in
        let base = Float((i / 4) * 37 + 20) / 255, step = Float(i % 2) / 255
        return SIMD4(base + step, 1 - base - step, Float(i / 4) / 4 + step * Float(i % 4), Float(i % 4 + 1) / 4)
    }

    typealias Ran = M2VMSAAFetchProbe.Ran

    /// r_sid4 (no methods) until raw draws it (a state's first uses may go to m2v while raw makes it), then the arms
    static func run(_ gpu: GPU, _ journal: ProbeJournal) async -> [(String, Ran)] {
        let pipeline: MTLRenderPipelineState
        do {
            pipeline = try gpu.render(fragment: "x1_sample_fragment", format: .bgra8Unorm, samples: 4)
        } catch {
            return [("r_sid4", Ran(verdict: "refused", note: "r_sid4: \(GPU.describe(error))"))]
        }
        var results: [(String, Ran)] = []
        var control: (verdict: String, picture: [UInt8]) = ("offraw", [])
        do {
            for _ in 0..<3 where control.verdict == "offraw" {
                control = try await draw("r_sid4", [], pipeline, gpu, journal)
            }
        } catch {
            return [("r_sid4", Ran(verdict: "error", note: "r_sid4: \(GPU.describe(error))"))]
        }
        results.append(("r_sid4", Ran(verdict: control.verdict)))
        for (name, methods) in arms {
            guard control.verdict == "ok" else {
                results.append((name, Ran(verdict: "skip")))
                continue
            }
            do {
                let drawn = try await draw(name, methods, pipeline, gpu, journal)
                var ran = Ran(verdict: drawn.verdict)
                let changed = zip(drawn.picture, control.picture).filter { $0 != $1 }.count
                if changed > 0 { ran.changed = "\(name):\(changed)" }
                results.append((name, ran))
            } catch {
                results.append((name, Ran(verdict: "error", note: "\(name): \(GPU.describe(error))")))
            }
        }
        return results
    }

    /// kill (the facade replaced its channel, or a command buffer failed), offraw (m2v drew it), noemit (the methods
    /// reached no draw), refused (the facade has no probe channel) or ok; and the resolved picture
    static func draw(
        _ name: String, _ methods: [(UInt32, UInt32)], _ pipeline: MTLRenderPipelineState, _ gpu: GPU,
        _ journal: ProbeJournal
    ) async throws -> (verdict: String, picture: [UInt8]) {
        let size = M2VMSAAFetchProbe.size
        let ms = try gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget], samples: 4, storage: .memoryless)
        let into = try gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget, .shaderRead])
        try await M2VMSAAFetchProbe.prefill(into, gpu)
        let reopens = RawFacade.counters(of: gpu.device)["reopens"] ?? 0
        if !methods.isEmpty,
            RawMethods.arm(gpu.device, before: methods, after: methods.map { ($0.0, held[$0.0] ?? 0) }) != nil
        {
            return ("refused", [])
        }
        journal.starting(name)
        let drawn: GPU.Timing
        do {
            drawn = try await gpu.run(tolerateError: true) { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = ms
                pass.colorAttachments[0].resolveTexture = into
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .multisampleResolve
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(pipeline)
                var sampleColors = Self.colors
                encoder.setFragmentBytes(&sampleColors, length: sampleColors.count * 16, index: 0)
                var corners = GPU.fullScreen
                encoder.setVertexBytes(&corners, length: 64, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
        } catch {
            _ = RawMethods.disarm(gpu.device)
            throw error
        }
        let emitted = methods.isEmpty ? 1 : RawMethods.disarm(gpu.device)
        let settled = try await ChannelKills.settle(gpu)
        let picture = try await gpu.read(into, bytesPerPixel: 4)
        let replaced = (RawFacade.counters(of: gpu.device)["reopens"] ?? 0) > reopens
        if replaced || drawn.path.hasPrefix("error:") || !settled.isEmpty { return ("kill", picture) }
        if drawn.path != "raw" { return ("offraw", picture) }
        return (emitted == 0 ? "noemit" : "ok", picture)
    }

    /// What the raw arms single out: pixel conflict detection over one pass per sample, and whether either AA control
    /// spares or kills it; control_<verdict> where twod_resolve's pass itself did not draw clean on raw
    static func lead(_ v: [String: String]) -> String {
        guard let control = v["r_sid4"] else { return "-" }
        guard control == "ok" else { return "control_" + control }
        var found: [String] = []
        if v["r_px"] == "kill" {
            if v["r_smp"] == "ok" { found.append("pixel_passes") }
            if v["r_px_aa0"] == "ok" { found.append("aa0_spares") }
            if v["r_px_csaa0"] == "ok" { found.append("csaa0_spares") }
        }
        for arm in ["r_smp", "r_aa0", "r_csaa0"] where v[arm] == "kill" {
            found.append(String(arm.dropFirst(2)) + "_kills")
        }
        return found.isEmpty ? "none" : found.joined(separator: "+")
    }
}
