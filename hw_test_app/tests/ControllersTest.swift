import Foundation
import GameController
import HWTestCore

/// The game controllers connected now, without asking anyone to press anything: how many, their names and profiles,
/// and the lowest battery level among them.
struct ControllersTest: HWTest {
    let id = "input.controllers"
    let name = "Game controllers connected now"
    let subsystem = Subsystem.input

    func run(_ context: TestContext) async throws -> Outcome {
        let controllers = await MainActor.run { GCController.controllers() }
        var metrics = Metrics()
        metrics.set("connected", controllers.count)
        guard !controllers.isEmpty else { return .skip("no game controller connected", metrics) }
        metrics.set("names", controllers.map { $0.vendorName ?? "?" }.joined(separator: ","))
        metrics.set("extended", controllers.filter { $0.extendedGamepad != nil }.count)
        if let level = controllers.compactMap({ $0.battery?.batteryLevel }).min() {
            metrics.set("battery_min", Double(level), digits: 2)
        }
        let profiled = controllers.allSatisfy { $0.extendedGamepad != nil || $0.microGamepad != nil }
        return .judged([("every controller has a gamepad profile", profiled)], metrics)
    }
}

/// What each connected Joy-Con publishes through GameController with nobody touching it: its thumbstick axes and its
/// A, B, X and Y buttons (`faces=` counts them per Joy-Con). A stick resting off centre means its calibration (from the
/// Joy-Con's SPI flash) is wrong; a face button reading pressed means its report bytes land in the wrong place.
struct JoyConInputTest: HWTest {
    let id = "input.joycon"
    let name = "Joy-Con sticks and face buttons at rest"
    let subsystem = Subsystem.input

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

    static func isJoyCon(_ controller: GCController) -> Bool {
        (controller.vendorName ?? "").contains("Joy-Con") || controller.productCategory.contains("Joy-Con")
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let readings = await MainActor.run { GCController.controllers().filter(Self.isJoyCon).map(Reading.init) }
        var metrics = Metrics()
        metrics.set("joycons", readings.count)
        guard !readings.isEmpty else { return .skip("no Joy-Con connected", metrics) }
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
