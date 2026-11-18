import Foundation

public enum Stats {
    /// Nearest-rank percentile (q in 0...1) of unsorted values; 0 for none.
    public static func percentile(_ values: [Double], _ q: Double) -> Double {
        guard !values.isEmpty else { return 0 }
        let sorted = values.sorted()
        let rank = Int((q * Double(sorted.count)).rounded(.up)) - 1
        return sorted[min(sorted.count - 1, max(0, rank))]
    }

    public static func mean(_ values: [Double]) -> Double {
        values.isEmpty ? 0 : values.reduce(0, +) / Double(values.count)
    }

    /// Frames a display missed: each interval of n periods (rounded) hides n - 1 of them.
    public static func droppedFrames(intervals: [Double], period: Double) -> Int {
        intervals.reduce(0) { $0 + max(0, Int(($1 / period).rounded()) - 1) }
    }

    public static func megabytesPerSecond(bytes: Int, seconds: Double) -> Double {
        seconds > 0 ? Double(bytes) / seconds / 1_000_000 : 0
    }
}
