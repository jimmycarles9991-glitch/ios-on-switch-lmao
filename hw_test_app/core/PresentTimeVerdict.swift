import Foundation

/// display.present_at_time's reading of Metal's contract (MTLDrawable.h): presentAtTime: shows the drawable at the
/// display refresh at its host time, presentedTime is the host time it went on screen (0 only when it was never shown
/// or was skipped), and the presented handler runs once it is on screen. The test's targets sit on the display link's
/// refresh grid, so a drawable shown on time has an offset (presentedTime minus target) within half a period of 0.
public enum PresentTimeVerdict {
    public static let period = 1.0 / 60

    /// One drawable, in CACurrentMediaTime's seconds: its commit, the time it asked for (0 for a plain present), what
    /// presentedTime said and when its presented handler ran (0 if it never did).
    public struct Frame: Sendable, Equatable {
        public let committed: Double
        public let target: Double
        public let presented: Double
        public let handled: Double

        public init(committed: Double, target: Double, presented: Double = 0, handled: Double = 0) {
            self.committed = committed
            self.target = target
            self.presented = presented
            self.handled = handled
        }

        /// presentedTime minus the target in milliseconds, nil when the drawable has no presentedTime.
        public var offsetMs: Double? { presented > 0 ? (presented - target) * 1000 : nil }
        /// presentedTime minus the commit in milliseconds, nil when the drawable has no presentedTime.
        public var latencyMs: Double? { presented > 0 ? (presented - committed) * 1000 : nil }
        public var leadMs: Double { (target - committed) * 1000 }
    }

    public struct Summary: Equatable {
        public let submitted, timed, handled, early, late: Int
        public let leadMinMs: Double

        public init(_ frames: [Frame]) {
            let half = PresentTimeVerdict.period * 1000 / 2
            let offsets = frames.compactMap(\.offsetMs)
            submitted = frames.count
            timed = offsets.count
            handled = frames.filter { $0.handled > 0 }.count
            early = offsets.filter { $0 < -half }.count
            late = offsets.filter { $0 > half }.count
            leadMinMs = frames.map(\.leadMs).min() ?? 0
        }
    }

    /// Each drawable's offset in milliseconds to one decimal, "x" for one with no presentedTime, in commit order.
    public static func offsets(_ frames: [Frame]) -> String {
        frames.map { $0.offsetMs.map { String(format: "%.1f", $0) } ?? "x" }.joined(separator: ",")
    }

    /// "shown/submitted" for a control phase.
    public static func shown(_ frames: [Frame]) -> String {
        "\(frames.compactMap(\.offsetMs).count)/\(frames.count)"
    }

    /// The timed phase's checks. None passes on an empty or late-armed run: every drawable must have been asked for
    /// more than a period after its commit, and every one must come back with a time.
    public static func checks(_ frames: [Frame], minimum: Int) -> [(String, Bool)] {
        let s = Summary(frames)
        return [
            ("submitted>=\(minimum)", s.submitted >= minimum),
            ("every_target_a_period_ahead", s.submitted > 0 && s.leadMinMs > period * 1000),
            ("handled==submitted", s.handled == s.submitted),
            ("timed==submitted", s.timed == s.submitted),
            ("early==0", s.early == 0),
            ("late==0", s.late == 0),
        ]
    }
}
