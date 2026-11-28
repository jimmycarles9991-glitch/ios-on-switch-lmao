import HWTestKit
import HWTestRegistry
import SwiftUI
@testable import SwitchHWTest27Tests

@main
struct SwitchHWTest27: App {
    @StateObject private var suite = Suite(
        tests: Registry.all + Tests27.all + Registry.last + Registry.recovery + X1Probes.last + Registry.firstTouch,
        ios: 27
    )

    var body: some Scene {
        WindowGroup {
            ContentView(suite: suite)
        }
    }
}
