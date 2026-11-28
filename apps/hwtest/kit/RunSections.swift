import HWTestCore
import SwiftUI

/// The list while a run goes: the running test pinned at the top, the last few finished newest first under it, and
/// the next few to come folded at the bottom, so following a run never needs a scroll. Every result is in the list by
/// subsystem once the run ends.
struct RunSections: View {
    @ObservedObject var list: RunList
    @State private var upNextShown = false

    var body: some View {
        Section("In progress") {
            if let row = list.current { RunningRow(row: row) }
        }
        Section("Done (\(list.progress.finished)), latest") {
            ForEach(list.done) { TestRowView(row: $0, compact: true) }
        }
        Section {
            DisclosureGroup("Up next: \(list.remainingCount)", isExpanded: $upNextShown) {
                ForEach(list.upNext) { TestRowView(row: $0, compact: true) }
            }
        }
    }
}

/// No elapsed-time counter: a timer redrawing the list would take the main thread from the test that runs.
struct RunningRow: View {
    @ObservedObject var row: RowState

    var body: some View {
        HStack(spacing: 12) {
            Text("RUN")
                .font(.caption2.bold().monospaced())
                .foregroundStyle(.secondary)
                .frame(width: 52)
            VStack(alignment: .leading, spacing: 3) {
                Text(row.test.name).font(.headline).lineLimit(1)
                Text("\(row.test.id) · \(row.test.subsystem.rawValue) · since \(started)")
                    .font(.caption.monospacedDigit()).foregroundStyle(.secondary).lineLimit(1)
            }
        }
        .frame(height: 44, alignment: .leading)
        .padding(.vertical, 4)
    }

    private var started: String {
        (row.started ?? Date()).formatted(date: .omitted, time: .standard)
    }
}
