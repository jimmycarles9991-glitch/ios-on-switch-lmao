import Foundation

public enum InterlockSpinBound {
    public static func accepts(_ times: [Double], baseline: [Double]) -> Bool {
        guard !times.isEmpty, !baseline.isEmpty,
            (times + baseline).allSatisfy({ $0.isFinite && $0 > 0 })
        else { return false }
        let limit = 4 * Stats.percentile(baseline, 0.5)
        return times.allSatisfy { $0 <= limit }
    }
}
