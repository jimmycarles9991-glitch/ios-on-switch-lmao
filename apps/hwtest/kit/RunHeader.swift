import HWTestCore
import SwiftUI

/// The top of the list: the run's progress while it runs, then one big PASS / FAIL count and what failed.
struct RunHeader: View {
    @ObservedObject var suite: Suite
    @ObservedObject var list: RunList

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if !suite.notice.isEmpty {
                Label(suite.notice, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            }
            summaryContent
            if !suite.running, let pending = suite.pendingRun {
                Text("Unfinished run: \(pending.results.count) of \(pending.summary.planned) tests completed")
                    .font(.subheadline).foregroundStyle(.secondary)
                if let error = suite.resumeError {
                    Text(error).font(.footnote).foregroundStyle(.red)
                }
            }
        }
        .padding(.vertical, 6)
        if !suite.running, suite.pendingRun != nil {
            Button {
                suite.continueRun()
            } label: {
                Label("Continue run", systemImage: "play.fill").font(.headline)
            }
            .disabled(!suite.idle)
            Button("Start new run") { suite.startNewRun() }
                .disabled(!suite.idle)
        }
    }

    @ViewBuilder private var summaryContent: some View {
        if suite.running {
            progress(list.progress)
            failures(list.progress.failed)
            if suite.reportSaved, let report = suite.report { reportPath(report) }
        } else if let report = suite.report {
            counts(report)
            failures(report.results.filter { $0.verdict == .fail }.map(\.id))
            if suite.reportSaved {
                reportPath(report)
            } else {
                Text("Ad-hoc results (unfinished run kept saved)").font(.caption2).foregroundStyle(.secondary)
            }
        } else if let pending = suite.pendingRun {
            counts(pending)
            failures(pending.results.filter { $0.verdict == .fail }.map(\.id))
            reportPath(pending)
        } else {
            startPrompt
        }
    }

    private var startPrompt: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("No test run yet.").font(.headline)
            Text("Start a full run, or pick Interactive, Bench, or any test below.")
                .font(.subheadline).foregroundStyle(.secondary)
            Button {
                suite.requestRunAll()
            } label: {
                Label("Start", systemImage: "play.fill")
            }
            .buttonStyle(.borderedProminent)
            .disabled(!suite.idle)
        }
    }

    private func reportPath(_ report: Report) -> some View {
        Text("Report: Library/Application Support/hwtest/\(report.fileName)")
            .font(.caption2).foregroundStyle(.secondary)
    }

    func progress(_ progress: RunList.Progress) -> some View {
        let planned = max(progress.planned, 1)
        return VStack(alignment: .leading, spacing: 6) {
            ProgressView(value: Double(min(progress.finished, planned)), total: Double(planned))
            Text("\(progress.finished) of \(progress.planned) · \(list.current?.test.name ?? "")")
                .font(.headline).lineLimit(1)
            Text("\(progress.pass) pass · \(progress.fail) fail · \(progress.skip) skip so far")
                .font(.footnote).foregroundStyle(.secondary)
        }
    }

    func counts(_ report: Report) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 24) {
            count(report.summary.pass, "PASS", .green)
            count(report.summary.fail, "FAIL", report.summary.fail > 0 ? .red : .secondary)
            count(report.summary.skip, "SKIP", .orange)
            if report.summary.crashed > 0 { count(report.summary.crashed, "CRASHED", .red) }
        }
    }

    func count(_ value: Int, _ label: String, _ color: Color) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("\(value)").font(.system(size: 56, weight: .bold, design: .rounded)).foregroundStyle(color)
            Text(label).font(.caption.bold()).foregroundStyle(.secondary)
        }
    }

    @ViewBuilder func failures(_ failed: [String]) -> some View {
        if !failed.isEmpty {
            Text("Failed: " + failed.prefix(12).joined(separator: ", ") + (failed.count > 12 ? ", …" : ""))
                .font(.footnote.monospaced()).foregroundStyle(.red)
        }
    }
}
