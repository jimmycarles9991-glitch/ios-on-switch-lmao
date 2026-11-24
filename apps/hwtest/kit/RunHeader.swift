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
            if let report = suite.report {
                if suite.running {
                    progress(list.progress)
                    failures(list.progress.failed)
                } else {
                    counts(report)
                    failures(report.results.filter { $0.verdict == .fail }.map(\.id))
                }
                Text("Report: Library/Application Support/hwtest/\(report.fileName)")
                    .font(.caption2).foregroundStyle(.secondary)
            } else {
                Text("Tap Run All. The run needs nothing else from you.").font(.headline)
            }
        }
        .padding(.vertical, 6)
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
