import HWTestKit
import SwiftUI

@main
struct SwitchHWTest17: App {
    @StateObject private var suite = Suite(tests: Registry.all, ios: 17)

    var body: some Scene {
        WindowGroup {
            ContentView(suite: suite)
        }
    }
}
