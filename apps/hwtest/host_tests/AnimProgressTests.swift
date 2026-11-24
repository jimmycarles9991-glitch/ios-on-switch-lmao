import Foundation
import HWTestCore
import XCTest

final class AnimProgressTests: XCTestCase {
    static let duration = 0.3, period = 1.0 / 60

    /// A 300 ms move evaluated at every `step` seconds from 5.0: by the clock, or by a fixed 1/60 s a frame.
    static func samples(_ curve: AnimCurve, step: Double, byClock: Bool) -> [AnimSample] {
        (0 ..< 40).map { frame in
            let time = 5.0 + Double(frame) * step
            let elapsed = byClock ? time - 5.0 : Double(frame) * period
            return AnimSample(time: time, progress: curve.value(elapsed / duration))
        }
    }

    func testAClockDrivenMoveKeepsToItsCurveAtAnyFrameRate() {
        for curve in [AnimCurve.linear, .spring] {
            for step in [Self.period, 2 * Self.period, 3 * Self.period] {
                let error = AnimProgress.worstError(
                    Self.samples(curve, step: step, byClock: true), curve: curve, duration: Self.duration,
                    slack: Self.period
                )
                XCTAssertLessThan(error, 0.001, "\(curve) every \(step) s")
            }
        }
    }

    /// The suspect for Spotlight's twice-as-long pull: an engine that advances a frame's worth whatever time passed.
    func testAFixedStepAt20FramesASecondFallsBehind() {
        for curve in [AnimCurve.linear, .spring] {
            let error = AnimProgress.worstError(
                Self.samples(curve, step: 3 * Self.period, byClock: false), curve: curve, duration: Self.duration,
                slack: Self.period
            )
            XCTAssertGreaterThan(error, 0.2, "\(curve)")
        }
    }

    /// A value evaluated for a frame up to one period from the clock is within the slack.
    func testAFramesOwnTimestampWithinAPeriodIsAllowed() {
        let samples = (0 ..< 18).map { frame -> AnimSample in
            let time = 1.0 + Double(frame) * Self.period
            return AnimSample(time: time, progress: AnimCurve.linear.value((time - 1.0 + Self.period) / Self.duration))
        }
        XCTAssertLessThan(
            AnimProgress.worstError(samples, curve: .linear, duration: Self.duration, slack: Self.period), 0.001
        )
    }

    func testTheSpringReachesItsEndSoonAfterItsDuration() {
        XCTAssertEqual(AnimCurve.spring.value(1), 0.986, accuracy: 0.001)
        XCTAssertEqual(AnimCurve.spring.time(of: 0.99), 1.057, accuracy: 0.002)
        XCTAssertEqual(AnimCurve.linear.time(of: 0.5), 0.5, accuracy: 1e-9)
        XCTAssertEqual(AnimCurve.linear.time(of: 1), 1, accuracy: 1e-9)
        XCTAssertEqual(AnimCurve.spring.time(of: 1), 2)
        XCTAssertEqual(AnimProgress.nominal(.linear, from: 0.05, to: 1, duration: 0.3), 0.285, accuracy: 1e-9)
    }

    /// A move held back by a blocked thread and then run in full keeps to its curve from where it began.
    func testAMoveThatBeganLateIsJudgedFromItsFirstValue() {
        let samples = [AnimSample(time: 5.1, progress: 0)]
            + Self.samples(.linear, step: Self.period, byClock: true).map {
                AnimSample(time: $0.time + 0.3, progress: $0.progress)
            }.filter { $0.progress > 0 }
        XCTAssertLessThan(
            AnimProgress.worstError(
                AnimProgress.moving(samples), curve: .linear, duration: Self.duration, slack: Self.period
            ), 0.001
        )
    }
}
