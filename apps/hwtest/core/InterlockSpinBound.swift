import Foundation

/// gpu.gm20b.interlock_spin's bound on the ticket wait: `layers` overlapping interlocked layers may cost at most
/// `layers` lone ones (the median of `lone`, one layer's runs, fixed costs included), so a layer that waits on the one
/// before it costs no more than one that waits on nothing. A missing or non-GPU time fails.
public enum InterlockSpinBound {
    public static func accepts(_ times: [Double], layers: Int, lone: [Double]) -> Bool {
        guard layers > 0, !times.isEmpty, !lone.isEmpty, (times + lone).allSatisfy({ $0.isFinite && $0 > 0 })
        else { return false }
        let limit = Double(layers) * Stats.percentile(lone, 0.5)
        return times.allSatisfy { $0 <= limit }
    }
}
