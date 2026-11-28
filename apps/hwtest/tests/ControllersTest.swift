import Foundation
import GameController
import HWTestCore
import HWTestKit

/// The game controllers connected: how many, their names and profiles, and the lowest battery level among them. Rail
/// Joy-Cons are a game controller only from a gamepad press (SwitchJoyConUse.h), so with none connected the test asks
/// for one.
struct ControllersTest: HWTest {
    let id = "input.controllers"
    let name = "Game controllers connected"
    let subsystem = Subsystem.input
    let interactive = true

    func run(_ context: TestContext) async throws -> Outcome {
        let controllers = await Self.connected(context)
        var metrics = Metrics()
        metrics.set("connected", controllers.count)
        guard !controllers.isEmpty
        else { return .skip(ControllerSkip.reason(railsRunning: Self.railsRunning()), metrics) }
        metrics.set("names", controllers.map { $0.vendorName ?? "?" }.joined(separator: ","))
        metrics.set("extended", controllers.filter { $0.extendedGamepad != nil }.count)
        if let level = controllers.compactMap({ $0.battery?.batteryLevel }).min() {
            metrics.set("battery_min", Double(level), digits: 2)
        }
        let profiled = controllers.allSatisfy { $0.extendedGamepad != nil || $0.microGamepad != nil }
        return .judged([("every controller has a gamepad profile", profiled)], metrics)
    }

    /// The controllers connected, after asking for the press that publishes the rail Joy-Cons when there are none
    static func connected(_ context: TestContext) async -> [GCController] {
        let now = await MainActor.run { GCController.controllers() }
        guard now.isEmpty else { return now }
        var observer: NSObjectProtocol?
        let attached: GCController? = await context.interact(
            "Press a button or push a stick on a Joy-Con (or connect a controller)", timeout: 30
        ) { waiter in
            observer = NotificationCenter.default.addObserver(
                forName: .GCControllerDidConnect, object: nil, queue: .main
            ) { waiter.resume($0.object as? GCController) }
        }
        if let observer { NotificationCenter.default.removeObserver(observer) }
        guard attached != nil else { return [] }
        return await MainActor.run { GCController.controllers() }
    }

    static func railsRunning() -> [String]? {
        guard case .value(let rails) = RailTest.read() else { return nil }
        return rails.filter(\.value.running).map(\.key)
    }
}

/// What each Joy-Con publishes through GameController at rest: its thumbstick axes and its A, B, X and Y buttons
/// (`faces=` counts them per Joy-Con). A rail Joy-Con is a game controller only from a gamepad press
/// (SwitchJoyConUse.h), so the test asks for one press, then for hands off, and reads once nothing has changed for a
/// second. A stick resting off centre means its calibration (from the Joy-Con's SPI flash) is wrong; a face button
/// reading pressed means its report bytes land in the wrong place.
struct JoyConInputTest: HWTest {
    let id = "input.joycon"
    let name = "Joy-Con sticks and face buttons at rest"
    let subsystem = Subsystem.input
    let interactive = true

    static let faceButtons: Set<String> = [GCInputButtonA, GCInputButtonB, GCInputButtonX, GCInputButtonY]
    static let thumbsticks: Set<String> = [GCInputLeftThumbstick, GCInputRightThumbstick]
    static let restRadius: Float = 0.25

    struct Reading {
        let name: String
        let sticks: [String: SIMD2<Float>]
        let pressedFaces: [String]
        let faceCount: Int

        init(_ controller: GCController) {
            let profile = controller.physicalInputProfile
            name = controller.vendorName ?? controller.productCategory
            sticks = profile.dpads.filter { JoyConInputTest.thumbsticks.contains($0.key) }.mapValues { stick in
                SIMD2(stick.xAxis.value, stick.yAxis.value)
            }
            let faces = profile.buttons.filter { JoyConInputTest.faceButtons.contains($0.key) }
            faceCount = faces.count
            pressedFaces = faces.filter { $0.value.isPressed }.map(\.key)
        }

        var centred: Bool {
            sticks.values.allSatisfy {
                abs($0.x) <= JoyConInputTest.restRadius && abs($0.y) <= JoyConInputTest.restRadius
            }
        }
    }

    /// True once no input of `joycons` has changed for a second after the hands-off prompt
    static func still(_ context: TestContext, _ joycons: [GCController]) async -> Bool {
        let quiet: Bool? = await context.interact("Let go of the Joy-Cons", timeout: 15) { waiter in
            var pending: DispatchWorkItem?
            let rearm = {
                pending?.cancel()
                let item = DispatchWorkItem { waiter.resume(true) }
                pending = item
                DispatchQueue.main.asyncAfter(deadline: .now() + 1, execute: item)
            }
            for joycon in joycons { joycon.physicalInputProfile.valueDidChangeHandler = { _, _ in rearm() } }
            rearm()
        }
        await MainActor.run { for joycon in joycons { joycon.physicalInputProfile.valueDidChangeHandler = nil } }
        return quiet == true
    }

    static func isJoyCon(_ controller: GCController) -> Bool {
        (controller.vendorName ?? "").contains("Joy-Con") || controller.productCategory.contains("Joy-Con")
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let joycons = await ControllersTest.connected(context).filter(Self.isJoyCon)
        var metrics = Metrics()
        guard !joycons.isEmpty else {
            metrics.set("joycons", 0)
            return .skip(ControllerSkip.reason(railsRunning: ControllersTest.railsRunning()), metrics)
        }
        guard await Self.still(context, joycons) else { return .skip(
            "the Joy-Cons never stayed still for a second",
            metrics
        ) }
        let readings = await MainActor.run { joycons.map(Reading.init) }
        metrics.set("joycons", readings.count)
        metrics.set("names", readings.map(\.name).joined(separator: ","))
        let axes = readings.flatMap { reading in
            reading.sticks.sorted { $0.key < $1.key }.map { _, value in
                String(format: "%.2f/%.2f", value.x, value.y)
            }
        }
        metrics.set("sticks", axes.joined(separator: ","))
        let pressed = readings.flatMap(\.pressedFaces).sorted()
        metrics.set("faces", readings.map { String($0.faceCount) }.joined(separator: ","))
        metrics.set("faces_pressed", pressed.count)
        return .judged(
            [
                ("every Joy-Con publishes a thumbstick", readings.allSatisfy { !$0.sticks.isEmpty }),
                ("every Joy-Con publishes a face button", readings.allSatisfy { $0.faceCount > 0 }),
                ("thumbsticks rest within \(Self.restRadius) of centre", readings.allSatisfy(\.centred)),
                ("no face button reads pressed at rest", pressed.isEmpty),
            ],
            metrics
        )
    }
}
