import Foundation
import HWTestCore

/// Each row is observed on its own, so a test's result redraws its row, not the list. Changed on the main thread only.
final class RowState: ObservableObject, Identifiable {
    let test: any HWTest
    @Published var record: TestRecord?
    @Published var running = false
    @Published var started: Date?

    var id: String { test.id }

    init(test: any HWTest) {
        self.test = test
    }
}

/// Changed once a test, by `advance`.
@MainActor
final class RunList: ObservableObject {
    struct Progress: Equatable {
        var finished = 0
        var planned = 0
        var pass = 0
        var fail = 0
        var skip = 0
        var failed: [String] = []
    }

    struct SubsystemRows: Identifiable {
        let subsystem: Subsystem
        let rows: [RowState]

        var id: Subsystem { subsystem }
    }

    let rows: [RowState]
    let bySubsystem: [SubsystemRows]
    private let byID: [String: RowState]
    @Published private(set) var current: RowState?
    @Published private(set) var done: [RowState] = []
    @Published private(set) var upNext: [RowState] = []
    @Published private(set) var progress = Progress()
    private var verdicts: [String: Verdict] = [:]

    init(tests: [any HWTest]) {
        let rows = tests.map(RowState.init)
        self.rows = rows
        byID = Dictionary(rows.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        bySubsystem = Subsystem.allCases.map { subsystem in
            SubsystemRows(subsystem: subsystem, rows: rows.filter { $0.test.subsystem == subsystem })
        }
        .filter { !$0.rows.isEmpty }
    }

    func seed(_ report: Report) {
        verdicts = Dictionary(report.results.map { ($0.id, $0.verdict) }, uniquingKeysWith: { $1 })
        let summary = report.summary
        progress = Progress(
            finished: report.results.count, planned: summary.planned, pass: summary.pass, fail: summary.fail,
            skip: summary.skip, failed: report.results.filter { $0.verdict == .fail }.map(\.id)
        )
        let planned = Set(report.run.order)
        done = rows.filter { $0.record != nil && planned.contains($0.id) }
            .sorted { ($0.record?.finished ?? .distantPast) > ($1.record?.finished ?? .distantPast) }
        upNext = report.remaining.compactMap { byID[$0] }
        current = nil
    }

    func advance(_ finished: TestRecord?, next: String?) {
        if let finished, let row = byID[finished.id] {
            row.record = finished
            row.running = false
            count(finished.id, finished.outcome.verdict)
            done.removeAll { $0 === row }
            done.insert(row, at: 0)
        }
        let nextRow = next.flatMap { byID[$0] }
        if let nextRow {
            nextRow.running = true
            nextRow.started = Date()
            upNext.removeAll { $0 === nextRow }
        }
        current = nextRow
    }

    private func count(_ id: String, _ verdict: Verdict) {
        if let earlier = verdicts.updateValue(verdict, forKey: id) {
            tally(earlier, -1)
            if earlier == .fail { progress.failed.removeAll { $0 == id } }
        } else {
            progress.finished += 1
        }
        tally(verdict, 1)
        if verdict == .fail { progress.failed.append(id) }
    }

    private func tally(_ verdict: Verdict, _ change: Int) {
        switch verdict {
        case .pass: progress.pass += change
        case .fail: progress.fail += change
        case .skip: progress.skip += change
        }
    }
}
