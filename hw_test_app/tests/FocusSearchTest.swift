import GameController
import HWTestCore
import UIKit

/// With a game controller connected, what one focus update costs over a Settings-like list and why it finds no item:
/// UIKitCore 24B119 retries a programmatic update that resolves to nil about once a second
/// (-[_UIFocusUpdateThrottle didCreateProgrammaticFocusUpdateContext:]), and on this CPU each one took 130-195 ms in
/// pacing.list. `reasons` is UIFocusDebugger's own account of the scene and of the list's first cell. Each update is
/// split by ThreadCost: user_ms is UIKit's and SwiftUI's traversal, system_ms and the call counts what it asked of the
/// kernel (IOKit, HID, sysctl, Mach messages), blocked_ms the rest of the wall time. The update repeats as the throttle
/// repeats it, so the split is a mean of `updates`.
struct FocusSearchTest: HWTest {
    let id = "input.focus_search"
    let name = "Focus update over a list with a game controller connected"
    let subsystem = Subsystem.input
    let interactive = true

    func run(_ context: TestContext) async throws -> Outcome {
        if await MainActor.run(body: { GCController.controllers().isEmpty }) {
            var observer: NSObjectProtocol?
            let prompt = "Press a button on the Joy-Cons (or connect a controller)"
            let attached: GCController? = await context.interact(prompt) { waiter in
                observer = NotificationCenter.default.addObserver(
                    forName: .GCControllerDidConnect, object: nil, queue: .main
                ) { waiter.resume($0.object as? GCController) }
            }
            if let observer { NotificationCenter.default.removeObserver(observer) }
            guard attached != nil else { return .skip("no game controller connected") }
        }
        let table = await context.onStage(fullScreen: true) { stage in
            let table = SettingsList.make(stage.bounds)
            stage.addSubview(table)
            return table
        }
        await Snapshots.settle()
        let updates = 8
        let (system, ms, focused, reasons, cost) = await MainActor.run {
            () -> (Bool, Double, Bool, String, ThreadCost) in
            guard let focus = table.window.flatMap({ UIFocusSystem.focusSystem(for: $0) }) else {
                return (false, 0, false, "", ThreadCost())
            }
            let before = ThreadCost.sample()
            let start = CACurrentMediaTime()
            for _ in 0..<updates {
                focus.requestFocusUpdate(to: table)
                focus.updateFocusIfNeeded()
            }
            let elapsed = (CACurrentMediaTime() - start) * 1000 / Double(updates)
            let cost = ThreadCost.sample() - before
            var text = String(describing: UIFocusDebugger.status())
            if let cell = table.visibleCells.first {
                text += " " + String(describing: UIFocusDebugger.checkFocusability(for: cell))
            }
            return (true, elapsed, focus.focusedItem != nil, text, cost)
        }
        await context.clearStage()
        var metrics = Metrics()
        metrics.set("focus_system", system)
        metrics.set("update_ms", ms, digits: 1)
        metrics.set("updates", updates)
        let user = cost.userUs / 1000 / Double(updates), kernel = cost.systemUs / 1000 / Double(updates)
        metrics.set("user_ms", user, digits: 1)
        metrics.set("system_ms", kernel, digits: 1)
        metrics.set("blocked_ms", max(0, ms - user - kernel), digits: 1)
        metrics.set("mach_calls", cost.machCalls / updates)
        metrics.set("unix_calls", cost.unixCalls / updates)
        metrics.set("switches", cost.switches / updates)
        metrics.set("faults", cost.faults / updates)
        metrics.set("focused", focused)
        metrics.set("reasons", LogLine.fitted([reasons]))
        guard system else { return .skip("no focus system with a controller connected", metrics) }
        return .judged(
            [
                ("one focus update over the list takes under a frame", ms < 16.7),
                ("the update finds an item to focus", focused),
            ], metrics)
    }
}
