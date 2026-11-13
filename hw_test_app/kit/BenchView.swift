import HWTestCore
import SwiftUI
import UIKit

/// The bench loop's screen: it runs while it is open, with the display kept awake so the app is never suspended.
struct BenchView: View {
    let documents: URL
    let ios: Int
    let close: () -> Void
    @StateObject private var loop = BenchLoop()

    var body: some View {
        NavigationStack {
            List {
                Section("Device") {
                    row("Raw device", loop.status.device)
                    row("Self-check", loop.status.selfCheck)
                        .foregroundStyle(loop.status.selfCheck.hasPrefix("PASS") ? .green : .red)
                }
                Section("Inboxes") {
                    if loop.status.roots.isEmpty {
                        Text("None reachable yet").foregroundStyle(.secondary)
                    }
                    ForEach(loop.status.roots, id: \.self) { Text($0).font(.caption.monospaced()) }
                }
                Section("Batches") {
                    row("Now", loop.status.current)
                    row("Experiments run", "\(loop.status.finished)")
                    if !loop.status.last.isEmpty { Text(loop.status.last).font(.caption.monospaced()) }
                }
            }
            .navigationTitle("Bench loop")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Stop") {
                        loop.stop()
                        close()
                    }
                }
            }
        }
        .onAppear {
            UIApplication.shared.isIdleTimerDisabled = true
            loop.start(documents: documents, ios: ios)
        }
        .onDisappear {
            loop.stop()
            UIApplication.shared.isIdleTimerDisabled = false
        }
    }

    private func row(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.caption).foregroundStyle(.secondary)
            Text(value).font(.body.monospaced())
        }
    }
}
