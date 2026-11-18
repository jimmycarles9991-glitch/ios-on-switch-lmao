import SwiftUI
#if DEBUG
    import SwitchInstallerNative
#endif

struct InstallerView: View {
    @ObservedObject var model: SwitchInstallerModel
    @State private var choosingFile = false
    @State private var enteringLink = false
    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                sourceAction("Choose IPA", systemImage: "folder") {
                    #if DEBUG
                        SwitchImportDiagnostics.capture("picker-presented")
                    #endif
                    choosingFile = true
                }
                Divider().padding(.horizontal)
                sourceAction("Download from Link", systemImage: "link") {
                    enteringLink = true
                }
            }
            .disabled(model.phase.isBusy)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .navigationTitle("Installer")
            .navigationBarTitleDisplayMode(.large)
            .navigationDestination(
                isPresented: Binding(
                    get: { model.phase != .idle && !model.showsImportDialog },
                    set: { if !$0 && !model.showsImportDialog { model.dismissReview() } }
                )
            ) {
                InstallerReviewView(model: model)
            }
            .sheet(isPresented: $choosingFile) {
                InstallerFilePicker { result in
                    choosingFile = false
                    #if DEBUG
                        SwitchImportDiagnostics.capture("picker-completed")
                    #endif
                    switch result {
                    case .success(let url):
                        if let url { model.importFile(url) }
                    case .failure(let error):
                        model.report(error)
                    }
                }
            }
            .sheet(isPresented: $enteringLink) {
                DownloadLinkView { link in model.downloadLink(link) }
            }
            .alert(
                "Installer",
                isPresented: Binding(
                    get: { model.notice != nil },
                    set: { if !$0 { model.dismissNotice() } }
                )
            ) {
                Button("OK") { model.dismissNotice() }
            } message: {
                Text(model.notice ?? "")
            }
        }
        .tint(Color(uiColor: .systemBlue))
        .sheet(isPresented: importDialog) {
            VStack(spacing: 16) {
                Text(model.phase == .checking ? "Checking app…" : "Importing IPA…")
                    .font(.headline)
                if let name = model.importName {
                    Text(name).foregroundStyle(.secondary).lineLimit(2).truncationMode(.middle)
                }
                if model.phase == .importing, let fraction = model.copyFraction {
                    ProgressView(value: fraction)
                        .accessibilityLabel("Copying IPA")
                } else {
                    ProgressView()
                }
                Button("Cancel", role: .cancel) { model.cancel() }
            }
            .frame(maxWidth: 360)
            .padding()
            .presentationDetents([.medium])
            .interactiveDismissDisabled()
        }
    }

    private var importDialog: Binding<Bool> {
        Binding(
            get: { model.showsImportDialog && !choosingFile && !enteringLink },
            set: { if !$0 && model.showsImportDialog { model.cancel() } }
        )
    }

    private func sourceAction(_ title: String, systemImage: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label {
                Text(title).foregroundStyle(.primary)
            } icon: {
                Image(systemName: systemImage).foregroundStyle(.tint)
            }
            .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
            .padding(.horizontal)
            .padding(.vertical, 8)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}
