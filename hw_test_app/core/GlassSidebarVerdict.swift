import Foundation

/// ui.glass_sidebar's verdict over its phases (idle ones and moving ones, in run order) and the probe words read at
/// each phase boundary (`reads[i]` before phase i, `reads[i + 1]` after it). The render server publishes the words only
/// at its commits, so an idle phase's end reads what it published before the idle: its trimmed pool shows first in the
/// words the next moving phase publishes.
public enum GlassSidebarVerdict {
    public struct Phase: Sendable {
        public let name: String
        public let moving: Bool
        public let frames: Int
        /// Sequences of the frames whose sidebar was black, at their present or at the end of their time on screen.
        public let black: [UInt64]
        public let partial: Int
        /// The pool words read after each frame of a moving phase.
        public let pools: [UInt64]

        public init(
            name: String, moving: Bool, frames: Int = 0, black: [UInt64] = [], partial: Int = 0, pools: [UInt64] = []
        ) {
            self.name = name
            self.moving = moving
            self.frames = frames
            self.black = black
            self.partial = partial
            self.pools = pools
        }
    }

    /// The pool words published after the idle phase at `index`: those its next phase read, then the one read after
    /// that phase. Empty when the run stopped first.
    public static func pools(after index: Int, _ phases: [Phase], _ reads: [GlassMemWords]) -> [UInt64] {
        guard index + 2 < reads.count, index + 1 < phases.count else { return [] }
        return phases[index + 1].pools + [reads[index + 2].pool]
    }

    /// Whether the idle reclaim showed for the idle phase at `index`, against the pool read at its start; nil when the
    /// probe published nothing or the run stopped first.
    public static func reclaimed(_ index: Int, _ phases: [Phase], _ reads: [GlassMemWords]) -> Bool? {
        let later = pools(after: index, phases, reads)
        guard reads.contains(where: \.published), !later.isEmpty else { return nil }
        let start = GlassPool(word: reads[index].pool)
        return later.contains { GlassPool.reclaimed(from: start, to: GlassPool(word: $0)) }
    }

    /// `scene` holds what the test measured of its split view, logged after the totals; `stopped` names the phase
    /// whose read-back failed and why; `subject` is what the judged band shows (ui.glass_settings: its glass pane).
    public static func outcome(
        _ phases: [Phase], reads: [GlassMemWords], scene: Metrics, pngs: [String], stopped: String?,
        subject: String = "sidebar"
    ) -> Outcome {
        let probe = reads.contains(where: \.published)
        let moving = phases.filter(\.moving)
        let frames = moving.map(\.frames).reduce(0, +)
        let black = moving.map(\.black.count).reduce(0, +)
        let partial = moving.map(\.partial).reduce(0, +)
        let idles = phases.indices.filter { !phases[$0].moving }
        let reclaims = idles.map { (phases[$0].name, reclaimed($0, phases, reads)) }
        var metrics = Metrics()
        metrics.set("frames", frames)
        metrics.set("black", black)
        metrics.set("partial", partial)
        metrics.set("probe", probe ? "1" : "-")
        let known = reclaims.compactMap(\.1)
        metrics.set("reclaim", known.isEmpty ? "-" : known.allSatisfy { $0 } ? "yes" : "no")
        metrics.merge(scene)
        var reasons: [String] = []
        for (index, phase) in phases.enumerated() {
            if phase.moving {
                metrics.set(phase.name, "\(phase.frames)/\(phase.black.count)/\(phase.partial)")
                if let first = phase.black.first { metrics.set(phase.name + "_first", Int(first)) }
                if !phase.black.isEmpty {
                    reasons.append(
                        "\(phase.name): \(phase.black.count) of \(phase.frames) frames show the \(subject) black")
                }
            }
            guard probe, index + 1 < reads.count else { continue }
            let rise = GlassMemRise(reads[index].mem, reads[index + 1].mem)
            metrics.set(phase.name + "_mem", rise.text)
            if rise.faults { reasons.append("\(phase.name): glass-mem \(rise.text)") }
            if !phase.moving, let after = pools(after: index, phases, reads).first {
                metrics.set(
                    phase.name + "_pool", GlassPool(word: reads[index].pool).text + ">" + GlassPool(word: after).text)
            }
        }
        if partial > 0 { reasons.append("\(partial) frames show the \(subject) partly black") }
        metrics.set("pngs", pngs.isEmpty ? "-" : String(pngs.prefix(3).joined(separator: ",").prefix(120)))
        if let stopped { metrics.set("stopped", stopped) }
        if !reasons.isEmpty { return .fail(LogLine.fitted(reasons, budget: 160), metrics) }
        if frames == 0 { return .skip("the read-back caught no presented frame", metrics) }
        if let stopped { return .skip("the read-back stopped: \(stopped)", metrics) }
        return .pass(metrics)
    }
}
