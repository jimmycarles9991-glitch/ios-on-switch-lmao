import HWTestCore
import SwiftUI
import UIKit

public struct ContentView: View {
    @ObservedObject var suite: Suite
    @ObservedObject var prompter: Prompter
    @ObservedObject var stage: Stage
    @State private var benchShown = false

    public init(suite: Suite) {
        self.suite = suite
        prompter = suite.prompter
        stage = suite.stage
    }

    public var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                if stage.visible && !stage.fullScreen {
                    StageView(stage: stage).frame(height: 240)
                }
                List {
                    RunHeader(suite: suite)
                    GlassWatchRow()
                    if suite.running {
                        RunSections(suite: suite)
                    } else {
                        bySubsystem
                    }
                }
                .listStyle(.insetGrouped)
            }
            .navigationTitle("Hardware tests")
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Interactive") { suite.runInteractive() }
                        .disabled(suite.running || suite.pendingRun != nil)
                }
                ToolbarItem(placement: .topBarLeading) {
                    Button("Bench") { benchShown = true }
                        .disabled(suite.running || suite.pendingRun != nil)
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Run All") { suite.runAll() }
                        .disabled(suite.running || suite.pendingRun != nil).bold()
                }
            }
        }
        .overlay {
            if stage.visible && stage.fullScreen {
                StageView(stage: stage).ignoresSafeArea()
            }
        }
        .overlay(alignment: .bottom) {
            if let message = prompter.message {
                PromptCard(message: message) { prompter.skipTapped() }.padding()
            }
        }
        .onAppear { suite.launch() }
        .fullScreenCover(isPresented: $benchShown) {
            BenchView(documents: suite.documents, ios: suite.ios) { benchShown = false }
        }
        .sheet(isPresented: Binding(get: { suite.pendingRun != nil }, set: { _ in })) {
            VStack(alignment: .leading, spacing: 20) {
                Text("Unfinished test run").font(.title2.bold())
                if let run = suite.pendingRun {
                    Text(
                        "\(run.results.count) of \(run.summary.planned) tests completed. Continue this run, or cancel it and start a new one."
                    )
                }
                if let error = suite.resumeError {
                    Text(error).foregroundStyle(.red)
                }
                Button("Continue run") { suite.continueRun() }.buttonStyle(.borderedProminent)
                Button("Start new run") { suite.startNewRun() }.buttonStyle(.bordered)
            }
            .padding(24)
            .presentationDetents([.medium])
            .interactiveDismissDisabled()
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.didEnterBackgroundNotification)) { _ in
            suite.interrupted("backgrounded")
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willTerminateNotification)) { _ in
            suite.interrupted("closed")
        }
        .onReceive(NotificationCenter.default.publisher(for: UIScene.didDisconnectNotification)) { _ in
            suite.interrupted("closed")
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willEnterForegroundNotification)) { _ in
            suite.interrupted(nil)
        }
    }

    /// Every test by subsystem, between runs; tapping one reruns it.
    @ViewBuilder var bySubsystem: some View {
        ForEach(Subsystem.allCases, id: \.self) { subsystem in
            let rows = suite.rows.filter { $0.test.subsystem == subsystem }
            if !rows.isEmpty {
                Section(subsystem.rawValue) {
                    ForEach(rows) { row in
                        TestRow(row: row).contentShape(Rectangle()).onTapGesture {
                            if !suite.running && suite.pendingRun == nil { suite.rerun(row.id) }
                        }
                    }
                }
            }
        }
    }
}

struct TestRow: View {
    let row: Suite.Row

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            badge.frame(width: 52)
            VStack(alignment: .leading, spacing: 3) {
                Text(row.test.name).font(.body)
                Text(row.test.id + (row.test.interactive ? " · interactive" : ""))
                    .font(.caption).foregroundStyle(.secondary)
                if let outcome = row.record?.outcome {
                    if !outcome.metrics.keys.isEmpty {
                        Text(outcome.metrics.ordered.map { "\($0.0)=\($0.1.logText)" }.joined(separator: "  "))
                            .font(.caption.monospaced())
                    }
                    if !outcome.proof.isEmpty {
                        Text("hw: " + outcome.proof).font(.caption).foregroundStyle(.teal)
                    }
                    if !outcome.detail.isEmpty {
                        Text(outcome.detail).font(.caption).foregroundStyle(
                            outcome.verdict == .fail ? .red : .secondary)
                    }
                }
            }
        }
        .padding(.vertical, 2)
    }

    @ViewBuilder var badge: some View {
        if row.running {
            ProgressView()
        } else if let verdict = row.record?.outcome.verdict {
            Text(verdict.rawValue)
                .font(.caption.bold().monospaced())
                .padding(.horizontal, 6).padding(.vertical, 3)
                .background(color(verdict).opacity(0.2), in: RoundedRectangle(cornerRadius: 5))
                .foregroundStyle(color(verdict))
        } else {
            Text("—").foregroundStyle(.secondary)
        }
    }

    func color(_ verdict: Verdict) -> Color {
        switch verdict {
        case .pass: return .green
        case .fail: return .red
        case .skip: return .orange
        }
    }
}

struct PromptCard: View {
    let message: String
    let skip: () -> Void

    var body: some View {
        HStack(spacing: 16) {
            Image(systemName: "hand.raised.fill").font(.title2).foregroundStyle(.orange)
            Text(message).font(.headline).frame(maxWidth: .infinity, alignment: .leading)
            Button("Skip", action: skip).buttonStyle(.bordered)
        }
        .padding()
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
        .shadow(radius: 8)
    }
}

struct StageView: UIViewRepresentable {
    let stage: Stage

    func makeUIView(context: Context) -> UIView {
        let host = UIView()
        host.backgroundColor = .black
        return host
    }

    func updateUIView(_ host: UIView, context: Context) {
        let container = stage.container
        guard container.superview !== host else { return }
        container.removeFromSuperview()
        container.frame = host.bounds
        container.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        host.addSubview(container)
    }
}
