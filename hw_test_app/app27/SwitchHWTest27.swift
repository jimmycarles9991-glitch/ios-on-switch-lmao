import HWTestKit
import SwiftUI

@main
struct SwitchHWTest27: App {
    @StateObject private var suite = Suite(tests: Registry.all + Tests27.all, ios: 27)

    var body: some Scene {
        WindowGroup {
            ContentView(suite: suite)
        }
    }
}
