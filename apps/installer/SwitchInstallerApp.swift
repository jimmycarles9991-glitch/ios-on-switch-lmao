import SwiftUI
#if DEBUG
    import SwitchInstallerNative
#endif

@main
struct SwitchInstallerApp: App {
    @StateObject private var model = SwitchInstallerModel()

    var body: some Scene {
        WindowGroup {
            InstallerView(model: model)
            #if DEBUG
                .task {
                    SwitchImportDiagnostics.capture("launch")
                }
            #endif
                .onOpenURL {
                    #if DEBUG
                        SwitchImportDiagnostics.capture("URL-delivered")
                    #endif
                    model.importFile($0)
                }
        }
    }
}
