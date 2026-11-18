import SwiftUI
import SwitchInstallerInput

struct InstallerStatusView: View {
    @ObservedObject var model: SwitchInstallerModel

    var body: some View {
        switch model.phase {
        case .idle:
            EmptyView()
        case .importing:
            progress("Importing IPA…")
        case .checking:
            progress("Checking app…")
        case .downloading(let received, let expected):
            Section {
                VStack(alignment: .leading, spacing: 12) {
                    Text("Downloading IPA…").font(.headline)
                    if expected > 0 {
                        ProgressView(value: min(Double(received) / Double(expected), 1))
                        Text("\(bytes(received)) of \(bytes(expected))")
                            .font(.subheadline).foregroundStyle(.secondary).monospacedDigit()
                    } else {
                        ProgressView()
                        Text("\(bytes(received)) downloaded")
                            .font(.subheadline).foregroundStyle(.secondary).monospacedDigit()
                    }
                }
                .padding(.vertical, 8)
                Button("Cancel", role: .cancel) { model.cancel() }
            } footer: {
                Text("Keep Installer open until the download finishes.")
            }
        case .ready:
            Section {
                action(model.package?.isUpdate == true ? "Update App" : "Install App") {
                    model.install()
                }
            }
            .listRowSeparator(.hidden)
        case .installing:
            Section {
                InstallerProgressRow(progress: model.installProgress)
                    .listRowSeparator(.hidden)
            }
            .listSectionSeparator(.hidden)
        case .installed:
            Section {
                Label("Installed", systemImage: "checkmark.circle.fill").font(.headline).foregroundStyle(.green)
                action("Open App") { model.openInstalledApp() }
            }
            .listRowSeparator(.hidden)
        case .failed(let message, let retryInstallation):
            Section {
                Label("Couldn’t finish", systemImage: "exclamationmark.triangle").font(.headline)
                Text(message)
                if retryInstallation {
                    Button("Try Installation Again") { model.install() }
                }
                DisclosureGroup("Technical details") {
                    Text(model.technicalDetails).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                }
            }
            .listRowSeparator(.hidden)
        }
    }

    private func action(_ title: String, perform: @escaping () -> Void) -> some View {
        Button(action: perform) {
            Text(title).frame(maxWidth: .infinity, minHeight: 44)
        }
        .font(.headline)
        .buttonStyle(.borderedProminent)
    }

    private func progress(_ title: String) -> some View {
        Section {
            HStack(spacing: 12) {
                ProgressView()
                Text(title).font(.headline)
            }
            .padding(.vertical, 8)
            if model.phase.canCancel {
                Button("Cancel", role: .cancel) { model.cancel() }
            }
        }
    }

    private func bytes(_ count: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: count, countStyle: .file)
    }
}

struct InstallerProgressRow: View {
    let progress: InstallerProgress

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline) {
                Text(progress.title).font(.headline)
                Spacer()
                if let percent = progress.percent {
                    Text((Double(percent) / 100).formatted(.percent))
                        .font(.subheadline).monospacedDigit().foregroundStyle(.secondary)
                }
            }
            ProgressView(value: progress.percent.map(Double.init), total: 100)
                .progressViewStyle(.linear)
                .accessibilityHidden(true)
        }
        .padding(.vertical, 8)
        .accessibilityElement(children: .combine)
    }
}
