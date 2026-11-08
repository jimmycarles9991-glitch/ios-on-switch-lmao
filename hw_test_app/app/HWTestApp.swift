import SwiftUI

@main
struct HWTestApp: App {
    @StateObject private var suite = Suite(tests: Registry.all)

    var body: some Scene {
        WindowGroup {
            ContentView(suite: suite)
        }
    }
}
