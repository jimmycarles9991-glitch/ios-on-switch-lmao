import Foundation

public struct Continuation {
    public enum Cause: Equatable {
        case none
        case restarted(String)
        case interrupted(String, reason: String)
        case crashed(String)
    }

    public let report: Report
    public let order: [String]
    public let cause: Cause
    public let crash: TestRecord?
}

extension Report {
    public func continued(
        boot: String, at date: Date, identity: [String: String], available: Set<String>,
        record: (String, Outcome) -> TestRecord
    ) -> Continuation {
        var report = self
        var cause = Continuation.Cause.none
        var crash: TestRecord?
        if let current = report.run.current {
            if report.latestBootUUID != boot {
                report.requeueInterrupted()
                cause = .restarted(current)
            } else if let reason = report.run.interruption {
                report.requeueInterrupted()
                cause = .interrupted(current, reason: reason)
            } else {
                let made = record(current, Report.crashOutcome(current))
                report.record(made)
                crash = made
                cause = .crashed(current)
            }
        }
        report.interrupt(nil)
        report.resume(bootUUID: boot, at: date, identity: identity)
        for id in report.remaining where !available.contains(id) {
            report.record(record(id, .skip("test is not available in this build")))
        }
        var order = report.remaining
        if case .restarted(let current) = cause, let index = order.firstIndex(of: current) {
            order.append(order.remove(at: index))
        }
        if order.isEmpty { report.finish(at: date) }
        return Continuation(report: report, order: order, cause: cause, crash: crash)
    }
}
