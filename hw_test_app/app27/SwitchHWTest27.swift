import HWTestKit
import SwiftUI

@main
struct SwitchHWTest27: App {
    @StateObject private var suite = Suite(
        tests: Registry.all.filter { $0.id != "input.focus_search" } + [FocusSearch27Test()]
            + Tests27.all + Registry.last + Registry.recovery + X1Probes.last, ios: 27)

    var body: some Scene {
        WindowGroup {
            ContentView(suite: suite)
        }
    }
}
