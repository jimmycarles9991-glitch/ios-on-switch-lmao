import Foundation

/// gpu.ca.hot_states' check that the pixel interlock costs a Core Animation state no more than its class's factor over
/// the same state's draws when no fragment waits on another. Per state that interlocked on the raw device
/// (raw_ilk_<label> > 0): its full-screen draw over the seven before it (raw_us_<label>, an eighth of its command buffer)
/// against the same pixels drawn in the same run as eight disjoint bands (raw_apart_us_<label>), each less its share of
/// the pass with no draw (raw_pass_us_<label>), all timed by the raw path's GPU stamps. A fetching draw runs its state's
/// program inside the interlock either way (raw_fetch_order.h), so the ratio is what waiting on other fragments costs.
/// Whether it shows a ticket buffer that bypasses the GPU's L2 (stack-S-v2: TimgBvcm's overlapping draw 26.1-34.1 ms
/// against 2.4 ms cached, probe-v1) is unproven until a device run shows the uncontended draw unslowed by it. A time that
/// is the host's window (no GPU stamp) is no GPU time and fails the check, and so does an interlocked state with no
/// uncontended or pass time, or no stated factor.
public enum InterlockBound {
    /// The factor per class of interlocked state, by the label's texture function: TimgBvcm's colour-matrix blend that
    /// fetches its destination, and the glass's sample (Tgsa) beside its R32Float and RGBA16Float attachments.
    public static let factors: [(texture: String, factor: Double)] = [("timgbvcm", 2.0), ("tgsa", 2.0)]

    /// The draws of each timed command buffer (gpu.ca.hot_states)
    public static let draws = 8

    public static func factor(_ label: String) -> Double? {
        factors.first { label.contains("_" + $0.texture) }?.factor
    }

    /// Per interlocked state: raw_ilk_x_<label>, its overlapping draw's time over its uncontended one, both less the pass
    /// and as milliseconds (raw_ilk_ms_<label>, raw_apart_ms_<label>) for comparisons across builds, and a reason for
    /// each past its factor or that cannot be judged.
    public static func judge(_ metrics: inout Metrics) -> [String] {
        var reasons: [String] = []
        let derived = ["raw_ilk_x_", "raw_ilk_ms_"]
        for key in metrics.keys.sorted()
        where key.hasPrefix("raw_ilk_") && !derived.contains(where: { key.hasPrefix($0) }) {
            let label = String(key.dropFirst("raw_ilk_".count))
            guard let interlocked = value(metrics[key]), interlocked > 0, let raw = value(metrics["raw_us_" + label])
            else {
                continue
            }
            if metrics["raw_gpu_clock_" + label] == .text("host") {
                reasons.append("\(label): the raw interlocked draw's time is the host's window, not the GPU's")
                continue
            }
            guard let bands = value(metrics["raw_apart_us_" + label]),
                let pass = value(metrics["raw_pass_us_" + label]), bands > pass
            else {
                reasons.append("\(label): interlocked, with no uncontended time to hold it to")
                continue
            }
            let apart = bands - pass
            let overlapping = raw - pass / Double(draws)
            guard let factor = factor(label) else {
                reasons.append("\(label): interlocked, and its class has no stated factor")
                continue
            }
            let ratio = overlapping / apart
            metrics.set("raw_ilk_x_" + label, ratio, digits: 2)
            metrics.set("raw_ilk_ms_" + label, overlapping / 1000, digits: 3)
            metrics.set("raw_apart_ms_" + label, apart / 1000, digits: 3)
            if ratio > factor {
                let shown = MetricValue.format(ratio, digits: 1)
                reasons.append("\(label): the interlocked draw takes \(shown)x its uncontended time (limit \(factor)x)")
            }
        }
        return reasons
    }

    static func value(_ metric: MetricValue?) -> Double? {
        switch metric {
        case .integer(let value): return Double(value)
        case .number(let value, _): return value
        default: return nil
        }
    }
}
