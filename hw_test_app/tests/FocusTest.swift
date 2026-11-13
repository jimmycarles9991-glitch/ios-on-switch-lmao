import GameController
import HWTestCore
import UIKit

/// Whether UIKit runs a focus system with no keyboard and no game controller connected. UIKitCore 24B119 sets one up in
/// every scene while a hardware keyboard or a Game Pad / Joystick HID device exists, and its programmatic focus updates
/// then stall the main thread about once a second (drivers/SwitchJoyCon/SwitchJoyConUse.h). The rail Joy-Cons are a
/// game controller only from a gamepad press, so with nobody touching them this app has no focus system.
struct FocusIdleTest: HWTest {
    let id = "input.focus_idle"
    let name = "No focus system without a keyboard or game controller"
    let subsystem = Subsystem.input

    func run(_ context: TestContext) async throws -> Outcome {
        let (controllers, keyboard, focus) = await MainActor.run { () -> (Int, Bool, Bool) in
            let window = UIApplication.shared.connectedScenes.compactMap { ($0 as? UIWindowScene)?.keyWindow }.first
            let system = window.flatMap { UIFocusSystem.focusSystem(for: $0) }
            return (GCController.controllers().count, GCKeyboard.coalesced != nil, system != nil)
        }
        var metrics = Metrics()
        metrics.set("controllers", controllers)
        metrics.set("keyboard", keyboard)
        metrics.set("focus_system", focus)
        guard controllers == 0, !keyboard else {
            return .skip("a game controller or keyboard is connected", metrics)
        }
        return .judged([("no focus system without a keyboard or game controller", !focus)], metrics)
    }
}
