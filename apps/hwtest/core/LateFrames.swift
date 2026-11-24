import Foundation

/// The late frames of a run of presented times (seconds of CACurrentMediaTime's clock, which is mach absolute time):
/// every interval of more than one and a half periods, with the time it ended and where that time falls on a grid of
/// absolute time, 250 ms (15 periods at 60 Hz) and 500 ms, so that a cause on a fixed grid shows as one phase whatever
/// test it lands in.
public struct LateFrames: Equatable, Sendable {
    public static let gridMs = 250.0

    public struct Late: Equatable, Sendable {
        /// When the late frame was shown, in seconds
        public let at: Double
        /// Its interval in whole periods
        public let periods: Int
        /// `at` modulo 250 ms and 500 ms, in milliseconds
        public let phase: Double
        public let phase500: Double
    }

    public let late: [Late]
    public let intervals: Int
    public let periodMs: Double

    /// `times` in any order; zeros (a drawable never shown) are left out.
    public init(presented times: [Double], period: Double) {
        let shown = times.filter { $0 > 0 }.sorted()
        periodMs = period * 1000
        intervals = max(shown.count - 1, 0)
        var late: [Late] = []
        for (previous, time) in zip(shown, shown.dropFirst()) {
            let ms = (time - previous) * 1000
            guard ms > periodMs * 1.5 else { continue }
            let absoluteMs = time * 1000
            late.append(
                Late(
                    at: time, periods: Int((ms / periodMs).rounded()),
                    phase: absoluteMs.truncatingRemainder(dividingBy: Self.gridMs),
                    phase500: absoluteMs.truncatingRemainder(dividingBy: 2 * Self.gridMs)
                )
            )
        }
        self.late = late
    }

    /// The most late frames whose 250 ms phases lie within half a period of one another (on the circle): all of them
    /// when one cause on a 250 ms grid made every late frame.
    public var onGrid: Int {
        let half = periodMs / 2
        return late.map { one in
            late.filter { other in
                let d = abs(one.phase - other.phase)
                return min(d, Self.gridMs - d) <= half
            }.count
        }.max() ?? 0
    }

    /// "phase/periods" per late frame, 250 ms phases in whole milliseconds, comma-joined (at most `limit`); "-" for none
    public func phases(limit: Int = 16) -> String {
        guard !late.isEmpty else { return "-" }
        let shown = late.prefix(limit).map { "\(Int($0.phase))/\($0.periods)" }
        return shown.joined(separator: ",") + (late.count > limit ? ",+\(late.count - limit)" : "")
    }

    public func metrics(prefix: String = "") -> Metrics {
        var metrics = Metrics()
        metrics.set(prefix + "late", late.count)
        metrics.set(prefix + "on_grid", onGrid)
        metrics.set(prefix + "phases", phases())
        return metrics
    }
}
