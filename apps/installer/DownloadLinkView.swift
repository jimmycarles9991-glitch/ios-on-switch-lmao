import SwiftUI
import SwitchInstallerInput

struct DownloadLinkView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var link = ""
    @FocusState private var focused: Bool
    let download: (String) -> Void

    var body: some View {
        NavigationStack {
            List {
                Section {
                    TextField("https://example.com/app.ipa", text: $link, axis: .vertical)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .focused($focused)
                        .submitLabel(.go)
                        .onSubmit { submit() }
                        .accessibilityLabel("IPA download link")
                } footer: {
                    Text("""
                    Paste a direct HTTPS download link. For sites that require a login, \
                    download in Safari and choose the IPA from Files.
                    """)
                }
            }
            .listStyle(.plain)
            .navigationTitle("Download from Link")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Download") { submit() }
                        .disabled((try? InstallerInput.downloadURL(link)) == nil)
                }
            }
            .onAppear { focused = true }
        }
        .presentationDetents([.medium, .large])
    }

    private func submit() {
        guard (try? InstallerInput.downloadURL(link)) != nil else { return }
        download(link)
        dismiss()
    }
}
