import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.m2v.msaa_fetch: which state kills m2v's channel on gpu.ca.fetch_routes' multisampled pass (gr: DATA_ERROR 0x134,
/// INVALID_A_A_COMBINATION03, at DRAW_VERTEX_ARRAY of 36 vertices). On NVK m2v fetches a colour as a storage image in
/// the pixel interlock, read at [[sample_id]] at more than one sample, with no colour attachment; by NVK's source it
/// then shades one pass per sample (SET_HYBRID_ANTI_ALIAS_CONTROL) under pixel conflict detection
/// (SET_PIXEL_SHADER_INTERLOCK_CONTROL), moving to sample detection only for a pipeline whose sample shading is its own.
/// Each m2v arm's note gives (conflict detection, passes) as that reading predicts them. Every arm is journalled before
/// its commit; a kill counts where its command buffer failed or NVK logged a replaced channel since it began
/// (ChannelKills, kill<error> where the line names the kernel's error).
enum M2VMSAAFetchProbe {
    static let size = 64
    static let clear = MTLClearColor(red: 0.125, green: 0.25, blue: 0.5, alpha: 1)
    static let journalName = "gpu.m2v.msaa_fetch"

    struct Arm {
        let name: String
        let fragment: String
        var samples = 4
        var vertices = 36
        var fetches = false
        var blends = false
        var counts = false
        var target = true
    }

    /// In the order they run: the controls first, gpu.ca.fetch_routes' own pass last
    static let arms: [Arm] = [
        Arm(name: "plain4", fragment: "msaa_probe_plain_fragment"),  // (none, 1)
        Arm(name: "fetch1", fragment: "fetch_order_fragment", samples: 1, fetches: true),  // (pixel, 1), no target
        Arm(name: "sid4", fragment: "msaa_probe_sample_fragment"),  // (none, 4)
        Arm(name: "rog4", fragment: "msaa_probe_ordered_fragment"),  // (pixel, 1)
        // (none, 4), no target
        Arm(name: "empty4", fragment: "msaa_probe_count_fragment", counts: true, target: false),
        // a blended fetch is an input attachment, unordered within a draw: one triangle
        Arm(name: "fetch4b", fragment: "fetch_order_fragment", vertices: 3, fetches: true, blends: true),  // (none, 4)
        Arm(name: "fetch4ss", fragment: "msaa_probe_fetch_count_fragment", fetches: true, counts: true),  // (sample, 4)
        Arm(name: "rogsid4", fragment: "msaa_probe_ordered_sample_fragment"),  // (pixel, 4)
        Arm(name: "fetch2", fragment: "fetch_order_fragment", samples: 2, fetches: true),  // (pixel, 2), no target
        Arm(name: "fetch4t", fragment: "fetch_order_fragment", vertices: 3, fetches: true),  // (pixel, 4), no target
        Arm(name: "fetch4", fragment: "fetch_order_fragment", fetches: true),  // (pixel, 4), no target
    ]

    /// An arm's outcome: ok, kill[<error>], wrong (no kill, pixels or invocations off), refused (no pipeline), error,
    /// skip; and for the m2v arms whether the log read back the arm's own journal line
    struct Ran {
        var verdict = "ok"
        var first = ""
        var invocations = ""
        var changed = ""
        var note = ""
        var logSeen = true
        var logMilliseconds = 0.0
    }

    static let probe = X1Probe(
        id: "gpu.m2v.msaa_fetch", name: "m2v's MSAA framebuffer fetch kill: interlock, passes and AA controls by arm",
        probe: "m2v.msaa_fetch", risky: true, timeout: 300
    ) { gpu in
        let journal = ProbeJournal(probe: journalName)
        var results: [(String, Ran)] = []
        let m2vDevice =
            RawFacade.m2vDevice(of: gpu.device) ?? (RawFacade.kind(of: gpu.device) == "m2v" ? gpu.device : nil)
        if let m2vDevice {
            let m2v = try GPU.make(device: m2vDevice)
            for arm in arms { results.append((arm.name, await run(arm, m2v, journal))) }
        }
        if RawFacade.kind(of: gpu.device) == "raw" {
            results += await M2VMSAARawReplay.run(gpu, journal)
        }
        return finding(results, m2v: m2vDevice != nil)
    }

    static func run(_ arm: Arm, _ gpu: GPU, _ journal: ProbeJournal) async -> Ran {
        let pipeline: MTLRenderPipelineState
        do {
            pipeline = try gpu.render(
                vertex: "fetch_vertex", fragment: arm.fragment, format: arm.target ? .bgra8Unorm : .invalid,
                samples: arm.samples
            ) { descriptor in
                guard arm.blends, let attachment = descriptor.colorAttachments[0] else { return }
                attachment.isBlendingEnabled = true
                attachment.sourceRGBBlendFactor = .one
                attachment.sourceAlphaBlendFactor = .one
                attachment.destinationRGBBlendFactor = .zero
                attachment.destinationAlphaBlendFactor = .zero
            }
        } catch {
            return Ran(verdict: "refused", note: "\(arm.name): \(GPU.describe(error))")
        }
        var since: Date?
        do {
            return try await draw(arm, pipeline, gpu) {
                since = Date()
                journal.starting(arm.name)
            }
        } catch {
            var ran = Ran(verdict: "error", note: "\(arm.name): \(GPU.describe(error))")
            if let since {
                let scan = ChannelKills.scan(since: since, probe: journalName, step: arm.name)
                if let kill = scan.errors.first { ran.verdict = "kill\(kill)" }
                ran.logSeen = scan.seen
                ran.logMilliseconds = scan.milliseconds
            }
            return ran
        }
    }

    /// `begin` journals the arm: after its targets are made and filled, before its commit
    static func draw(
        _ arm: Arm, _ pipeline: MTLRenderPipelineState, _ gpu: GPU, begin: () -> Void
    ) async throws -> Ran {
        let resolved = try gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget, .shaderRead])
        let target =
            try arm.samples > 1
            ? gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget], samples: arm.samples) : resolved
        let count = try gpu.buffer([UInt32(0)])
        try await prefill(resolved, gpu)
        let since = Date()
        begin()
        let drawn = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            if arm.target {
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = clear
                pass.colorAttachments[0].storeAction = arm.samples > 1 ? .multisampleResolve : .store
                if arm.samples > 1 { pass.colorAttachments[0].resolveTexture = resolved }
            } else {
                pass.renderTargetWidth = size
                pass.renderTargetHeight = size
                pass.defaultRasterSampleCount = arm.samples
            }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            var zero: UInt32 = 0
            encoder.setFragmentBytes(&zero, length: 4, index: 0)
            encoder.setFragmentBuffer(count, offset: 0, index: 1)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: arm.vertices)
            encoder.endEncoding()
        }
        let settled = try await ChannelKills.settle(gpu)
        let picture = arm.target ? try await gpu.read(resolved, bytesPerPixel: 4) : []
        let scan = ChannelKills.scan(since: since, probe: journalName, step: arm.name)
        var ran = Ran(logSeen: scan.seen, logMilliseconds: scan.milliseconds)
        let triangles = arm.vertices / 3
        if arm.counts {
            let want = triangles * size * size * arm.samples
            let got = Int(count.contents().load(as: UInt32.self))
            ran.invocations = "\(arm.name):\(got)/\(want)"
            if got != want { ran.verdict = "wrong" }
        }
        if arm.target {
            let texel = arm.fetches ? CAFetchRouteScene.expected(Array(0..<triangles)) : stepColor(triangles - 1)
            if let first = firstDifference(picture, Patterns.repeated(texel, count: size * size), width: size) {
                ran.verdict = "wrong"
                ran.first = "\(arm.name)@\(first)"
            }
        }
        if let kill = scan.errors.first {
            ran.verdict = "kill\(kill)"
        } else if drawn.path.hasPrefix("error:") || !settled.isEmpty {
            ran.verdict = "kill"
        }
        return ran
    }

    /// Magenta, which no arm draws, in `texture` before the arm: a draw the kill took leaves it there, never an
    /// earlier arm's picture in reused memory
    static func prefill(_ texture: MTLTexture, _ gpu: GPU) async throws {
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 1, green: 0, blue: 1, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
    }

    /// The first BGRA8 texel of `got` that differs from `want`, as x,y:got>want, or nil when every texel matches
    static func firstDifference(_ got: [UInt8], _ want: [UInt8], width: Int) -> String? {
        for t in 0..<min(got.count, want.count) / 4 {
            let range = t * 4..<(t + 1) * 4
            guard got[range] != want[range] else { continue }
            let hex = { (bytes: ArraySlice<UInt8>) in bytes.map { String(format: "%02x", $0) }.joined() }
            return "\(t % width),\(t / width):\(hex(got[range]))>\(hex(want[range]))"
        }
        return nil
    }

    /// msaa_probe_step's colour over nothing, stored as BGRA8
    static func stepColor(_ step: Int) -> [UInt8] {
        let next = [(step * 3 + 1) % 7, (step * 5 + 2) % 7, (step + 3) % 7].map { Float($0) / 8 }
        return TexelFormat.bgra8Unorm.encode(next + [1])
    }

    static func killed(_ verdicts: [String: String], _ arm: String) -> Bool {
        verdicts[arm]?.hasPrefix("kill") ?? false
    }

    static func drew(_ verdicts: [String: String], _ arm: String) -> Bool {
        killed(verdicts, arm) || ["ok", "wrong"].contains(verdicts[arm] ?? "")
    }

    /// Which state the m2v arms single out, by the arms that killed the channel
    static func lead(_ v: [String: String]) -> String {
        if !["fetch4", "fetch2", "fetch4t"].contains(where: { killed(v, $0) }) { return "no_kill" }
        if killed(v, "sid4") || killed(v, "fetch4b") { return "passes_without_interlock" }
        if killed(v, "empty4") && killed(v, "fetch4ss") { return "attachmentless_passes" }
        if killed(v, "rog4") { return "pixel_interlock_multisampled" }
        if drew(v, "fetch4ss") && !killed(v, "fetch4ss") && (killed(v, "rogsid4") || !drew(v, "rogsid4")) {
            return "pixel_interlock_passes"
        }
        return "other"
    }

    static func finding(_ results: [(String, Ran)], m2v: Bool) -> ProbeFinding {
        let verdicts = Dictionary(results.map { ($0.0, $0.1.verdict) }, uniquingKeysWith: { $1 })
        var metrics = Metrics()
        metrics.set("arms", results.map { "\($0.0):\($0.1.verdict)" }.joined(separator: ","))
        let killers = results.map(\.0).filter { killed(verdicts, $0) }
        metrics.set("killed_by", killers.isEmpty ? "-" : killers.joined(separator: ","))
        metrics.set("lead", m2v ? lead(verdicts) : "-")
        metrics.set("raw_lead", M2VMSAARawReplay.lead(verdicts))
        let invocations = results.map(\.1.invocations).filter { !$0.isEmpty }
        metrics.set("inv", invocations.isEmpty ? "-" : invocations.joined(separator: ","))
        if let first = results.first(where: { !$0.1.first.isEmpty }) { metrics.set("first_wrong", first.1.first) }
        let changed = results.map(\.1.changed).filter { !$0.isEmpty }
        metrics.set("raw_changed", changed.isEmpty ? "-" : changed.joined(separator: ","))
        let blind = results.filter { !$0.1.logSeen }.map(\.0)
        metrics.set("kill_log", blind.isEmpty ? "read" : "blind:" + blind.joined(separator: ","))
        metrics.set("log_ms", results.map(\.1.logMilliseconds).reduce(0, +), digits: 0)
        let notes = results.map(\.1.note).filter { !$0.isEmpty }
        guard m2v else { return ProbeFinding(.inconclusive, metrics, note: "no m2v device") }
        let fetches = ["fetch4", "fetch2", "fetch4t"]
        if fetches.contains(where: { killed(verdicts, $0) || verdicts[$0] == "wrong" }) {
            return ProbeFinding(.fail, metrics, note: LogLine.fitted(notes))
        }
        let verdict: ClaimVerdict = fetches.allSatisfy { verdicts[$0] == "ok" } ? .pass : .inconclusive
        return ProbeFinding(verdict, metrics, note: LogLine.fitted(notes))
    }
}
