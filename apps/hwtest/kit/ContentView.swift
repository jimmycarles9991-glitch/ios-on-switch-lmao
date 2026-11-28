import HWTestCore
import SwiftUI
import UIKit

public struct ContentView: View {
    @ObservedObject var suite: Suite
    @ObservedObject var prompter: Prompter
    @ObservedObject var stage: Stage
    @State private var benchShown = false
    @AppStorage(LongRuns.defaultsKey) private var longRuns = false

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
                    RunHeader(suite: suite, list: suite.list)
                    if suite.running {
                        RunSections(list: suite.list)
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
                        .disabled(!suite.idle)
                }
                ToolbarItem(placement: .topBarLeading) {
                    Button("Bench") { benchShown = true }
                        .disabled(!suite.idle)
                }
                ToolbarItem(placement: .topBarLeading) {
                    Toggle("Long runs", isOn: $longRuns).toggleStyle(.button).disabled(!suite.idle)
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button(suite.report == nil && suite.pendingRun == nil ? "Start" : "Run All") {
                        suite.requestRunAll()
                    }
                    .disabled(!suite.idle).bold()
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
        .sheet(
            isPresented: Binding(
                get: { suite.resumePromptShown },
                set: { if !$0 { suite.dismissPendingRun() } }
            )
        ) {
            UnfinishedRunSheet(suite: suite)
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
        ForEach(suite.list.bySubsystem) { group in
            Section(group.subsystem.rawValue) {
                ForEach(group.rows) { row in
                    TestRowView(row: row).contentShape(Rectangle()).onTapGesture {
                        if suite.idle { suite.rerun(row.id) }
                    }
                }
            }
        }
    }
}

struct UnfinishedRunSheet: View {
    @ObservedObject var suite: Suite

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Unfinished test run").font(.title2.bold())
            if let run = suite.pendingRun {
                Text(
                    "\(run.results.count) of \(run.summary.planned) tests completed. "
                        + "Continue this run, start a new one, or cancel to browse and run individual tests."
                )
            }
            if let error = suite.resumeError {
                Text(error).foregroundStyle(.red)
            }
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 12) { buttons }
                VStack(alignment: .leading, spacing: 10) { buttons }
            }
        }
        .padding(24)
        .presentationDetents([.medium, .large])
        .interactiveDismissDisabled(suite.busy)
    }

    @ViewBuilder private var buttons: some View {
        Button("Continue run") { suite.continueRun() }.buttonStyle(.borderedProminent).disabled(suite.busy)
        Button("Start new run") { suite.startNewRun() }.buttonStyle(.bordered).disabled(suite.busy)
        Button("Cancel", role: .cancel) { suite.dismissPendingRun() }.buttonStyle(.bordered).disabled(suite.busy)
    }
}

struct TestRowView: View {
    @ObservedObject var row: RowState
    /// A row of the list while a run goes: one line each, at a fixed height, so neither a test boundary nor a scene turn
    /// lays out its full metrics (hundreds for some tests) or self-sizes it. On 25 Nov anim.timing's 90° turn spent
    /// 536 ms of this app's main thread in trait propagation and list cell sizing.
    var compact = false
    static let compactHeight: CGFloat = 76

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            badge.frame(width: 52)
            VStack(alignment: .leading, spacing: 3) {
                Text(row.test.name).font(.body).lineLimit(compact ? 1 : nil)
                Text(row.test.id + (row.test.interactive ? " · interactive" : ""))
                    .font(.caption).foregroundStyle(.secondary).lineLimit(compact ? 1 : nil)
                if let outcome = row.record?.outcome {
                    if !outcome.metrics.keys.isEmpty {
                        Text(
                            outcome.metrics.ordered.prefix(compact ? 8 : .max).map { "\($0.0)=\($0.1.logText)" }
                                .joined(separator: "  ")
                        )
                        .font(.caption.monospaced()).lineLimit(compact ? 1 : nil)
                    }
                    if !outcome.proof.isEmpty && !compact {
                        Text("hw: " + outcome.proof).font(.caption).foregroundStyle(.teal)
                    }
                    if !outcome.detail.isEmpty {
                        Text(outcome.detail).font(.caption).lineLimit(compact ? 1 : nil).foregroundStyle(
                            outcome.verdict == .fail ? .red : .secondary
                        )
                    }
                }
            }
        }
        .frame(height: compact ? Self.compactHeight : nil, alignment: .topLeading)
        .padding(.vertical, 2)
    }

    @ViewBuilder var badge: some View {
        if row.running {
            Text("RUN").font(.caption2.bold().monospaced()).foregroundStyle(.secondary)
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
