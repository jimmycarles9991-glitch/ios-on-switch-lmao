import Foundation

/// anim.timing's judgement of an animation's own values: each value an animation's code set, against the curve it was
/// asked for at the time that passed since it started. An engine that steps its time by a fixed amount per frame falls
/// behind at 20 to 30 frames a second; one that reads the clock keeps within a frame of the curve.
public enum AnimCurve: Sendable {
    /// Progress proportional to time
    case linear
    /// A critically damped spring whose response is the duration (SwiftUI's spring(duration:bounce: 0)):
    /// 1 - (1 + wt) e^(-wt), w = 2 pi / duration
    case spring

    /// Progress at `x`, the time since the start in durations
    public func value(_ x: Double) -> Double {
        guard x > 0 else { return 0 }
        switch self {
        case .linear: return min(x, 1)
        case .spring:
            let w = 2 * Double.pi * x
            return 1 - (1 + w) * exp(-w)
        }
    }

    /// The time in durations at which the curve reaches `progress` (the curves only rise); 2 when it has not by then
    public func time(of progress: Double) -> Double {
        guard progress > 0 else { return 0 }
        var (low, high) = (0.0, 2.0)
        guard value(high) >= progress else { return high }
        for _ in 0 ..< 48 {
            let mid = (low + high) / 2
            if value(mid) < progress { low = mid } else { high = mid }
        }
        return high
    }
}

public struct AnimSample: Equatable, Sendable {
    /// When the animation's code set the value, in seconds
    public let time: Double
    public let progress: Double

    public init(time: Double, progress: Double) {
        self.time = time
        self.progress = progress
    }
}

public enum AnimProgress {
    /// The values from the first that moved off the start: a move held back (by a blocked thread, or until its
    /// transaction committed) has values at 0 before it began.
    public static func moving(_ samples: [AnimSample]) -> [AnimSample] {
        Array(samples.drop { $0.progress <= 0 })
    }

    /// The time the curve takes from `from` to `to`, in seconds
    public static func nominal(_ curve: AnimCurve, from: Double, to: Double, duration: Double) -> Double {
        (curve.time(of: to) - curve.time(of: from)) * duration
    }

    /// The worst amount by which a value lay outside the curve's progress over the frame around its time: the start
    /// is placed where the curve gives the first value, and each value may belong to any time within `slack` seconds
    /// of when it was set (an engine evaluates a frame for its own timestamp, up to a period from the clock).
    public static func worstError(
        _ samples: [AnimSample], curve: AnimCurve, duration: Double, slack: Double
    ) -> Double {
        guard let first = samples.first, duration > 0 else { return 0 }
        let start = first.time - curve.time(of: first.progress) * duration
        var worst = 0.0
        for sample in samples {
            let x = (sample.time - start) / duration
            let low = curve.value(x - slack / duration), high = curve.value(x + slack / duration)
            worst = max(worst, low - sample.progress, sample.progress - high)
        }
        return worst
    }
}
