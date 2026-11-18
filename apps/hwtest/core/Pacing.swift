import Foundation

/// Frame pacing over a series of frame times (display-link timestamps or drawables' presented times): every interval
/// as a whole number of display periods, in a histogram of 1, 2, 3 and 4-or-more periods.
public struct Pacing: Equatable, Sendable {
    public static let buckets = 4

    public let frames: Int
    public let periodMilliseconds: Double
    public let intervals: [Double]
    public let histogram: [Int]

    /// `times` in seconds, increasing; zeros (a drawable never presented) are left out of the intervals.
    public init(times: [Double], period: Double) {
        let shown = times.filter { $0 > 0 }
        frames = shown.count
        periodMilliseconds = period * 1000
        intervals = zip(shown.dropFirst(), shown).map { ($0 - $1) * 1000 }
        var histogram = [Int](repeating: 0, count: Self.buckets)
        for interval in intervals {
            let periods = Int((interval / (period * 1000)).rounded())
            histogram[min(max(periods, 1), Self.buckets) - 1] += 1
        }
        self.histogram = histogram
    }

    /// Frames the display showed nothing new for: an interval of n periods hides n - 1.
    public var missed: Int { Stats.droppedFrames(intervals: intervals, period: periodMilliseconds) }

    /// Frames shown per second over the measured span.
    public var fps: Double {
        let span = intervals.reduce(0, +)
        return span > 0 ? Double(intervals.count) / span * 1000 : 0
    }

    /// "598/2/0/0": intervals of 1, 2, 3 and 4+ periods.
    public var histogramText: String { histogram.map(String.init).joined(separator: "/") }

    public func metrics(prefix: String = "") -> Metrics {
        var metrics = Metrics()
        metrics.set(prefix + "frames", frames)
        metrics.set(prefix + "fps", fps, digits: 1)
        metrics.set(prefix + "missed", missed)
        metrics.set(prefix + "hist", histogramText)
        metrics.set(prefix + "p50_ms", Stats.percentile(intervals, 0.5), digits: 2)
        metrics.set(prefix + "p95_ms", Stats.percentile(intervals, 0.95), digits: 2)
        metrics.set(prefix + "p99_ms", Stats.percentile(intervals, 0.99), digits: 2)
        metrics.set(prefix + "max_ms", intervals.max() ?? 0, digits: 2)
        return metrics
    }
}
