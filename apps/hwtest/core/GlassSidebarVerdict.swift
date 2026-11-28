import Foundation

public enum GlassSidebarVerdict {
    public struct Phase: Sendable {
        public let name: String
        public let moving: Bool
        public let frames: Int
        public let black: [UInt64]
        public let partial: Int

        public init(name: String, moving: Bool, frames: Int = 0, black: [UInt64] = [], partial: Int = 0) {
            self.name = name
            self.moving = moving
            self.frames = frames
            self.black = black
            self.partial = partial
        }
    }

    public static func outcome(
        _ phases: [Phase], scene: Metrics, pngs: [String], stopped: String?, subject: String = "sidebar"
    ) -> Outcome {
        let moving = phases.filter(\.moving)
        let frames = moving.map(\.frames).reduce(0, +)
        let black = moving.map(\.black.count).reduce(0, +)
        let partial = moving.map(\.partial).reduce(0, +)
        var metrics = Metrics()
        metrics.set("frames", frames)
        metrics.set("black", black)
        metrics.set("partial", partial)
        metrics.merge(scene)
        var reasons: [String] = []
        for phase in phases where phase.moving {
            metrics.set(phase.name, "\(phase.frames)/\(phase.black.count)/\(phase.partial)")
            if let first = phase.black.first { metrics.set(phase.name + "_first", Int(first)) }
            if !phase.black.isEmpty {
                reasons.append(
                    "\(phase.name): \(phase.black.count) of \(phase.frames) frames show the \(subject) black"
                )
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
