import Foundation
import HWTestCore
import UIKit
import notify

struct IdentityTest: HWTest {
    let id = "system.identity"
    let name = "Device identity"
    let subsystem = Subsystem.system

    func run(_ context: TestContext) async throws -> Outcome {
        let info = await MainActor.run { DeviceInfo.current() }
        let idiom = await MainActor.run { UIDevice.current.userInterfaceIdiom }
        var metrics = Metrics()
        for key in ["model", "machine", "board", "system", "build"] { metrics.set(key, info[key] ?? "") }
        metrics.set("memory_mb", Int(ProcessInfo.processInfo.physicalMemory >> 20))
        return .judged(
            [
                ("UIDevice says iPad", info["model"] == "iPad" && idiom == .pad),
                ("hw.machine is an iPad identity", info["machine"]?.hasPrefix("iPad") == true),
                ("a build version", info["build"]?.isEmpty == false),
            ], metrics)
    }
}

struct PowerStateTest: HWTest {
    let id = "system.power"
    let name = "Battery, thermal state and low power mode"
    let subsystem = Subsystem.system

    static let batteryStates = ["unknown", "unplugged", "charging", "full"]

    func run(_ context: TestContext) async throws -> Outcome {
        let (level, state) = await MainActor.run { () -> (Float, UIDevice.BatteryState) in
            UIDevice.current.isBatteryMonitoringEnabled = true
            return (UIDevice.current.batteryLevel, UIDevice.current.batteryState)
        }
        let thermal = ProcessInfo.processInfo.thermalState
        var metrics = Metrics()
        metrics.set("battery_level", Double(level), digits: 2)
        metrics.set("battery_state", Self.batteryStates[state.rawValue])
        metrics.set("thermal", ["nominal", "fair", "serious", "critical"][thermal.rawValue])
        metrics.set("low_power", ProcessInfo.processInfo.isLowPowerModeEnabled)
        return .judged(
            [
                ("battery state known", state != .unknown),
                ("battery level in 0...1", level >= 0 && level <= 1),
                ("not thermally critical", thermal != .critical),
            ], metrics)
    }
}

/// thermalmonitord's verdict on a console at rest: the thermal pressure it publishes (what dasd, audiomxd and the
/// charge and backlight caps follow) and the thermal state apps read from it. Boot 15 reached moderate pressure ten
/// minutes in with the SoC at 44 C, from a bad PMU calibration offset on the board thermistors.
struct ThermalTest: HWTest {
    let id = "system.thermal"
    let name = "Thermal pressure nominal at rest"
    let subsystem = Subsystem.system

    static let pressureKey = "com.apple.system.thermalpressurelevel"

    static func pressure() -> UInt64? {
        var token: Int32 = 0
        guard notify_register_check(pressureKey, &token) == NOTIFY_STATUS_OK else { return nil }
        defer { notify_cancel(token) }
        var state: UInt64 = 0
        return notify_get_state(token, &state) == NOTIFY_STATUS_OK ? state : nil
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let pressure = Self.pressure()
        let thermal = ProcessInfo.processInfo.thermalState
        var metrics = Metrics()
        metrics.set("pressure", pressure.map { String($0) } ?? "unread")
        metrics.set("thermal", CPUClock.thermal[thermal.rawValue])
        return .judged(
            [
                ("thermal pressure published", pressure != nil),
                ("thermal pressure nominal", pressure == 0),
                ("thermal state nominal", thermal == .nominal),
            ], metrics)
    }
}
