import HWTestCore
import SwiftUI

/// The list while a run goes: the running test pinned at the top, the finished ones newest first under it, and the
/// ones still to come folded at the bottom, so following a run never needs a scroll.
struct RunSections: View {
    @ObservedObject var list: RunList
    @State private var upNextShown = false

    var body: some View {
        Section("In progress") {
            if let row = list.current { RunningRow(row: row) }
        }
        Section("Done (\(list.done.count))") {
            ForEach(list.done) { TestRowView(row: $0) }
        }
        Section {
            DisclosureGroup("Up next: \(list.upNext.count)", isExpanded: $upNextShown) {
                ForEach(list.upNext) { TestRowView(row: $0) }
            }
        }
    }
}

/// No elapsed-time counter: a timer redrawing the list would take the main thread from the test that runs.
struct RunningRow: View {
    @ObservedObject var row: RowState

    var body: some View {
        HStack(spacing: 12) {
            ProgressView().frame(width: 52)
            VStack(alignment: .leading, spacing: 3) {
                Text(row.test.name).font(.headline)
                Text("\(row.test.id) · \(row.test.subsystem.rawValue) · since \(started)")
                    .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 4)
    }

    private var started: String {
        (row.started ?? Date()).formatted(date: .omitted, time: .standard)
    }
}
