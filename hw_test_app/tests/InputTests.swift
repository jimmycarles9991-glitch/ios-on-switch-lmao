import Foundation
import GameController
import HWTestCore
import UIKit

struct GameControllerTest: HWTest {
    let id = "input.gamecontroller"
    let name = "Game controllers (Joy-Cons): attach, buttons and stick"
    let subsystem = Subsystem.input
    let interactive = true
    var timeout: TimeInterval { 120 }

    func run(_ context: TestContext) async throws -> Outcome {
        var observer: NSObjectProtocol?
        if GCController.controllers().isEmpty {
            let prompt = "Press a button on the Joy-Cons (or connect a controller)"
            let attached: GCController? = await context.interact(prompt) { waiter in
                observer = NotificationCenter.default.addObserver(
                    forName: .GCControllerDidConnect, object: nil, queue: .main
                ) {
                    waiter.resume($0.object as? GCController)
                }
            }
            if let observer { NotificationCenter.default.removeObserver(observer) }
            guard attached != nil else { return .skip("no controller attached") }
        }
        guard
            let controller = GCController.controllers().first(where: { $0.extendedGamepad != nil })
                ?? GCController.controllers().first
        else {
            return .fail("GCControllerDidConnect without a controller")
        }
        let pressed: Set<String>? = await context.interact(
            "Press A, B, X and Y on \(controller.vendorName ?? "the controller")"
        ) { waiter in
            var seen = Set<String>()
            controller.physicalInputProfile.valueDidChangeHandler = { _, element in
                guard let button = element as? GCControllerButtonInput, button.isPressed else { return }
                seen.insert(button.localizedName ?? button.sfSymbolsName ?? "button")
                if seen.count >= 4 { waiter.resume(seen) }
            }
        }
        await MainActor.run { controller.physicalInputProfile.valueDidChangeHandler = nil }
        var metrics = Metrics()
        metrics.set("controllers", GCController.controllers().count)
        metrics.set("vendor", controller.vendorName ?? "-")
        metrics.set("category", controller.productCategory)
        metrics.set("extended", controller.extendedGamepad != nil)
        guard let pressed else { return .skip("buttons not pressed (skipped or timed out)", metrics) }
        metrics.set("buttons", pressed.sorted().joined(separator: ","))
        let pushed: Float? = await context.interact("Push the thumbstick all the way in any direction") { waiter in
            controller.physicalInputProfile.valueDidChangeHandler = { _, element in
                guard let stick = element as? GCControllerDirectionPad,
                    !stick.aliases.isDisjoint(with: JoyConInputTest.thumbsticks)
                else { return }
                let reach = max(abs(stick.xAxis.value), abs(stick.yAxis.value))
                if reach >= 0.8 { waiter.resume(reach) }
            }
        }
        await MainActor.run { controller.physicalInputProfile.valueDidChangeHandler = nil }
        guard let pushed else { return .skip("thumbstick not pushed (skipped or timed out)", metrics) }
        metrics.set("stick_reach", Double(pushed), digits: 2)
        return .pass(metrics, proof: controller.productCategory)
    }
}

struct KeyboardTest: HWTest {
    let id = "input.keyboard"
    let name = "Keyboard"
    let subsystem = Subsystem.input
    let interactive = true

    func run(_ context: TestContext) async throws -> Outcome {
        var observer: NSObjectProtocol?
        if GCKeyboard.coalesced == nil {
            let connected: Bool? = await context.interact("Connect a keyboard") { waiter in
                observer = NotificationCenter.default.addObserver(
                    forName: .GCKeyboardDidConnect, object: nil, queue: .main
                ) { _ in waiter.resume(true) }
            }
            if let observer { NotificationCenter.default.removeObserver(observer) }
            guard connected == true else { return .skip("no keyboard connected") }
        }
        guard let keyboard = GCKeyboard.coalesced?.keyboardInput else {
            return .fail("GCKeyboard.coalesced has no input")
        }
        let key: GCKeyCode? = await context.interact("Press any key") { waiter in
            keyboard.keyChangedHandler = { _, _, code, pressed in if pressed { waiter.resume(code) } }
        }
        await MainActor.run { keyboard.keyChangedHandler = nil }
        guard let key else { return .skip("no key pressed") }
        var metrics = Metrics()
        metrics.set("key_code", key.rawValue)
        return .pass(metrics)
    }
}

/// Touches recorded while a finger drags: the digitizer's sample rate from coalesced touches, and the delivery
/// latency from each touch's hardware timestamp to the moment UIKit hands it over.
final class TouchPad: UIView {
    var samples: [TimeInterval] = []
    var latencies: [Double] = []
    var finished: (() -> Void)?

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first else { return }
        let now = CACurrentMediaTime()
        for coalesced in event?.coalescedTouches(for: touch) ?? [touch] { samples.append(coalesced.timestamp) }
        latencies.append((now - touch.timestamp) * 1000)
        if let first = samples.first, now - first > 3 { finished?() }
    }
}

struct TouchTest: HWTest {
    let id = "input.touch"
    let name = "Touch sample rate and latency"
    let subsystem = Subsystem.input
    let interactive = true

    func run(_ context: TestContext) async throws -> Outcome {
        let pad = await context.onStage(fullScreen: true) { stage -> TouchPad in
            let pad = TouchPad(frame: stage.bounds)
            pad.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            pad.backgroundColor = UIColor(white: 0.12, alpha: 1)
            stage.addSubview(pad)
            return pad
        }
        let done: Bool? = await context.interact("Drag a finger around the screen for three seconds") { waiter in
            pad.finished = { waiter.resume(true) }
        }
        let (samples, latencies) = await MainActor.run { (pad.samples, pad.latencies) }
        guard done == true else { return .skip("no drag (skipped or timed out)") }
        let intervals = zip(samples.dropFirst(), samples).map { $0 - $1 }.filter { $0 > 0 }
        let rate = intervals.isEmpty ? 0 : 1 / Stats.percentile(intervals, 0.5)
        var metrics = Metrics()
        metrics.set("samples", samples.count)
        metrics.set("rate_hz", rate, digits: 1)
        metrics.set("latency_p50_ms", Stats.percentile(latencies, 0.5), digits: 2)
        metrics.set("latency_p99_ms", Stats.percentile(latencies, 0.99), digits: 2)
        return .judged([("at least 60 Hz of touch samples", rate >= 60)], metrics)
    }
}
