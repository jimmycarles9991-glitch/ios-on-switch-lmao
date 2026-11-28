import Foundation

public struct RunSession: Equatable {
    public enum LaunchOutcome: Equatable {
        case empty
        case showLast(Report)
        case promptUnfinished(Report)
    }

    public enum RunTarget: Equatable {
        case recorded(Report)
        case ephemeral(Report)

        public var report: Report {
            switch self {
            case .recorded(let report), .ephemeral(let report): return report
            }
        }

        public var persists: Bool {
            switch self {
            case .recorded: return true
            case .ephemeral: return false
            }
        }
    }

    public enum RunAllDecision: Equatable {
        case startFresh
        case promptUnfinished
    }

    public private(set) var report: Report?
    public private(set) var reportSaved: Bool
    public private(set) var pendingRun: Report?
    public private(set) var resumePromptShown: Bool

    public init() {
        report = nil
        reportSaved = false
        pendingRun = nil
        resumePromptShown = false
    }

    public mutating func applyLaunch(recent: [Report], boot: String) -> LaunchOutcome {
        if let interrupted = Report.interrupted(recent) {
            pendingRun = interrupted
            resumePromptShown = true
            report = nil
            reportSaved = false
            return .promptUnfinished(interrupted)
        }
        if let last = recent.last(where: { $0.latestBootUUID == boot }) {
            pendingRun = nil
            resumePromptShown = false
            report = last
            reportSaved = true
            return .showLast(last)
        }
        pendingRun = nil
        resumePromptShown = false
        report = nil
        reportSaved = false
        return .empty
    }

    public mutating func dismissResumePrompt() {
        resumePromptShown = false
    }

    public mutating func acceptPendingChoice(_ chosen: Report) {
        pendingRun = nil
        resumePromptShown = false
        report = chosen
        reportSaved = true
    }

    public mutating func requestRunAll() -> RunAllDecision {
        if pendingRun != nil {
            resumePromptShown = true
            return .promptUnfinished
        }
        return .startFresh
    }

    public mutating func beginFreshRun(_ fresh: Report) {
        report = fresh
        reportSaved = true
    }

    /// Plans `ids` into the session's active report. While an unfinished run is still pending on disk, ad-hoc batches
    /// stay in memory so they neither mutate nor supersede that journal.
    public mutating func planBatch(_ ids: [String], makeReport: () -> Report) -> RunTarget {
        var planned = report ?? makeReport()
        planned.plan(ids)
        report = planned
        if pendingRun != nil {
            reportSaved = false
            return .ephemeral(planned)
        }
        reportSaved = true
        return .recorded(planned)
    }

    public mutating func finishBatch(_ final: Report?) {
        if let final { report = final }
    }
}
