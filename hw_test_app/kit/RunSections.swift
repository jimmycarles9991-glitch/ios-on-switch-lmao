import HWTestCore
import SwiftUI

/// The list while a run goes: the running test pinned at the top, the finished ones newest first under it, and the
/// ones still to come folded at the bottom, so following a run never needs a scroll.
struct RunSections: View {
    @ObservedObject var suite: Suite
    @State private var upNextShown = false

    var body: some View {
        let planned = Set(suite.report?.run.order ?? [])
        let done = suite.rows.filter { $0.record != nil && planned.contains($0.id) }
            .sorted { ($0.record?.finished ?? .distantPast) > ($1.record?.finished ?? .distantPast) }
        let upNext = (suite.report?.remaining ?? []).compactMap { id in suite.rows.first { $0.id == id } }
        Section("In progress") {
            ForEach(suite.rows.filter(\.running)) { RunningRow(row: $0) }
        }
        Section("Done (\(done.count))") {
            ForEach(done) { TestRow(row: $0) }
        }
        Section {
            DisclosureGroup("Up next: \(upNext.count)", isExpanded: $upNextShown) {
                ForEach(upNext) { TestRow(row: $0) }
            }
        }
    }
}

struct RunningRow: View {
    let row: Suite.Row

    var body: some View {
        HStack(spacing: 12) {
            ProgressView().frame(width: 52)
            VStack(alignment: .leading, spacing: 3) {
                Text(row.test.name).font(.headline)
                TimelineView(.periodic(from: .now, by: 0.5)) { context in
                    let elapsed = row.started.map { context.date.timeIntervalSince($0) } ?? 0
                    Text("\(row.test.id) · \(row.test.subsystem.rawValue) · \(String(format: "%.1f", elapsed)) s")
                        .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                }
            }
        }
        .padding(.vertical, 4)
    }
}
